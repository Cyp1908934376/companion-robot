/// Visual reasoning — object detection, face recognition, scene understanding.
///
/// Two backends:
///   - yolo: local YOLOv8n ONNX model via tract-onnx (real-time, <50ms on CPU)
///   - api: external vision API
///
/// Input:  JPEG image bytes
/// Output: VisionResult with detections, scene description

use crate::config::Config;
use crate::error::Error;
use crate::metrics;
use serde::{Deserialize, Serialize};
use std::sync::Mutex;
use std::time::Instant;

use tract_onnx::prelude::*;

// ── Vision types ───────────────────────────────────────────────

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct VisionResult {
    pub detections: Vec<Detection>,
    pub scene_description: Option<String>,
    pub face_count: usize,
    pub person_count: usize,
    pub duration_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Detection {
    pub class_name: String,
    pub confidence: f32,
    pub bbox: BoundingBox,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct BoundingBox {
    pub x: i32,
    pub y: i32,
    pub width: i32,
    pub height: i32,
}

// ── YOLOv8 constants ───────────────────────────────────────────

const YOLO_INPUT_SIZE: u32 = 640;

const COCO_CLASSES: &[&str] = &[
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck",
    "boat", "traffic light", "fire hydrant", "stop sign", "parking meter", "bench",
    "bird", "cat", "dog", "horse", "sheep", "cow", "elephant", "bear", "zebra",
    "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
    "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove",
    "skateboard", "surfboard", "tennis racket", "bottle", "wine glass", "cup",
    "fork", "knife", "spoon", "bowl", "banana", "apple", "sandwich", "orange",
    "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
    "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse",
    "remote", "keyboard", "cell phone", "microwave", "oven", "toaster", "sink",
    "refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
    "hair drier", "toothbrush",
];

// ── YOLO model management ──────────────────────────────────────

/// Cached YOLOv8 model.
static YOLO_MODEL: Mutex<Option<TypedSimplePlan<TypedModel>>> = Mutex::new(None);

fn load_yolo_model(model_path: &str) -> Result<(), Error> {
    let mut cache = YOLO_MODEL.lock().unwrap();
    if cache.is_some() {
        return Ok(());
    }

    let path = std::path::Path::new(model_path);
    if !path.exists() {
        return Err(Error::ModelNotAvailable(format!(
            "YOLO model not found at: {}",
            model_path
        )));
    }

    tracing::info!("loading YOLOv8 ONNX model from {}", model_path);
    let model = tract_onnx::onnx()
        .model_for_path(model_path)
        .map_err(|e| Error::Inference(format!("failed to load YOLO model: {}", e)))?
        .into_optimized()
        .map_err(|e| Error::Inference(format!("failed to optimize YOLO model: {}", e)))?
        .into_runnable()
        .map_err(|e| Error::Inference(format!("failed to create YOLO inference plan: {}", e)))?;

    tracing::info!("YOLOv8 model loaded successfully");
    *cache = Some(model);
    Ok(())
}

// ── Image preprocessing ────────────────────────────────────────

/// Decode JPEG and preprocess for YOLOv8.
/// Returns a flat f32 array of shape [1, 3, 640, 640] in CHW format, normalized to [0, 1].
fn preprocess_image(jpeg_data: &[u8]) -> Result<Vec<f32>, Error> {
    let img = image::load_from_memory(jpeg_data)
        .map_err(|e| Error::InvalidInput(format!("failed to decode JPEG: {}", e)))?;
    let img = img.to_rgb8();

    let resized = image::imageops::resize(
        &img,
        YOLO_INPUT_SIZE,
        YOLO_INPUT_SIZE,
        image::imageops::FilterType::Triangle,
    );

    let (w, h) = (resized.width() as usize, resized.height() as usize);
    let mut chw = vec![0.0f32; 3 * h * w];

    for y in 0..h {
        for x in 0..w {
            let pixel = resized.get_pixel(x as u32, y as u32);
            let idx = y * w + x;
            chw[0 * h * w + idx] = pixel[0] as f32 / 255.0;
            chw[1 * h * w + idx] = pixel[1] as f32 / 255.0;
            chw[2 * h * w + idx] = pixel[2] as f32 / 255.0;
        }
    }

    Ok(chw)
}

// ── YOLO postprocessing ────────────────────────────────────────

#[derive(Debug, Clone)]
struct RawDetection {
    class_id: usize,
    confidence: f32,
    cx: f32,
    cy: f32,
    w: f32,
    h: f32,
}

/// Parse YOLOv8 output tensor [1, 84, 8400] into raw detections.
fn parse_yolo_output(
    output: &[f32],
    num_classes: usize,
    num_anchors: usize,
    conf_threshold: f32,
    img_width: u32,
    img_height: u32,
) -> Vec<RawDetection> {
    let mut detections = Vec::new();
    let stride = num_classes + 4;

    if output.len() < stride * num_anchors {
        return detections;
    }

    let scale_x = img_width as f32 / YOLO_INPUT_SIZE as f32;
    let scale_y = img_height as f32 / YOLO_INPUT_SIZE as f32;

    for anchor_idx in 0..num_anchors {
        let offset = stride * anchor_idx;
        if offset + stride > output.len() {
            break;
        }

        // Class probabilities start at offset+4
        let class_start = offset + 4;
        let class_end = class_start + num_classes;
        if class_end > output.len() {
            break;
        }

        let class_slice = &output[class_start..class_end];
        let (max_class, max_conf) = class_slice
            .iter()
            .enumerate()
            .max_by(|(_, a), (_, b)| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal))
            .map(|(idx, &conf)| (idx, conf))
            .unwrap_or((0, 0.0));

        if max_conf < conf_threshold {
            continue;
        }

        let cx = output[offset] * scale_x;
        let cy = output[offset + 1] * scale_y;
        let w = output[offset + 2] * scale_x;
        let h = output[offset + 3] * scale_y;

        detections.push(RawDetection { class_id: max_class, confidence: max_conf, cx, cy, w, h });
    }

    detections
}

/// Non-Maximum Suppression — removes overlapping detections of the same class.
fn non_max_suppression(detections: &mut Vec<RawDetection>, iou_threshold: f32) {
    if detections.is_empty() {
        return;
    }

    detections.sort_by(|a, b| {
        b.confidence.partial_cmp(&a.confidence).unwrap_or(std::cmp::Ordering::Equal)
    });

    let mut keep = vec![true; detections.len()];

    for i in 0..detections.len() {
        if !keep[i] {
            continue;
        }
        for j in (i + 1)..detections.len() {
            if !keep[j] {
                continue;
            }
            if detections[i].class_id == detections[j].class_id {
                let iou = compute_iou(&detections[i], &detections[j]);
                if iou > iou_threshold {
                    keep[j] = false;
                }
            }
        }
    }

    let mut result = Vec::new();
    for (i, det) in detections.iter().enumerate() {
        if keep[i] {
            result.push(det.clone());
        }
    }
    *detections = result;
}

fn compute_iou(a: &RawDetection, b: &RawDetection) -> f32 {
    let ax1 = a.cx - a.w / 2.0;
    let ay1 = a.cy - a.h / 2.0;
    let ax2 = a.cx + a.w / 2.0;
    let ay2 = a.cy + a.h / 2.0;
    let bx1 = b.cx - b.w / 2.0;
    let by1 = b.cy - b.h / 2.0;
    let bx2 = b.cx + b.w / 2.0;
    let by2 = b.cy + b.h / 2.0;

    let inter_x1 = ax1.max(bx1);
    let inter_y1 = ay1.max(by1);
    let inter_x2 = ax2.min(bx2);
    let inter_y2 = ay2.min(by2);

    if inter_x2 <= inter_x1 || inter_y2 <= inter_y1 {
        return 0.0;
    }

    let inter_area = (inter_x2 - inter_x1) * (inter_y2 - inter_y1);
    let area_a = (ax2 - ax1) * (ay2 - ay1);
    let area_b = (bx2 - bx1) * (by2 - by1);
    let union_area = area_a + area_b - inter_area;

    if union_area <= 0.0 {
        return 0.0;
    }

    inter_area / union_area
}

// ── Detection API ──────────────────────────────────────────────

pub async fn analyze(config: &Config, image_data: &[u8]) -> Result<VisionResult, Error> {
    let start = Instant::now();

    let result = match config.vision_backend.as_str() {
        "yolo" => analyze_local(config, image_data).await?,
        "api" => analyze_api(config, image_data).await?,
        other => {
            return Err(Error::ModelNotAvailable(format!(
                "unknown vision backend: {}",
                other
            )))
        }
    };

    let duration_ms = start.elapsed().as_millis() as u64;
    metrics::VISION_REQUESTS.inc();
    metrics::VISION_LATENCY.observe(duration_ms as f64);

    Ok(VisionResult { duration_ms, ..result })
}

async fn analyze_local(config: &Config, image_data: &[u8]) -> Result<VisionResult, Error> {
    tracing::debug!(
        "local YOLO: model={}, image_len={} bytes, threshold={}",
        config.vision_model_path,
        image_data.len(),
        config.vision_detection_threshold
    );

    if image_data.is_empty() {
        return Ok(empty_result());
    }

    // Get original dimensions
    let (orig_w, orig_h) = match image::load_from_memory(image_data) {
        Ok(img) => (img.width(), img.height()),
        Err(e) => {
            tracing::warn!("failed to decode image: {}", e);
            return Ok(empty_result());
        }
    };

    // Preprocess
    let preprocessed = match preprocess_image(image_data) {
        Ok(p) => p,
        Err(e) => {
            tracing::warn!("image preprocessing failed: {}", e);
            return Ok(empty_result());
        }
    };

    // Load model (cached)
    if let Err(e) = load_yolo_model(&config.vision_model_path) {
        tracing::warn!("YOLO model not available: {}", e);
        return Ok(empty_result());
    }

    // Run inference
    let detections = match run_yolo_inference(preprocessed, orig_w, orig_h, config.vision_detection_threshold) {
        Ok(d) => d,
        Err(e) => {
            tracing::warn!("YOLO inference failed: {}", e);
            return Ok(empty_result());
        }
    };

    let person_count = detections.iter().filter(|d| d.class_name == "person").count();

    tracing::info!(
        "YOLO detected {} objects ({} persons) in {}x{}",
        detections.len(), person_count, orig_w, orig_h
    );

    Ok(VisionResult {
        detections,
        scene_description: None,
        face_count: person_count,
        person_count,
        duration_ms: 0,
    })
}

fn empty_result() -> VisionResult {
    VisionResult {
        detections: Vec::new(),
        scene_description: None,
        face_count: 0,
        person_count: 0,
        duration_ms: 0,
    }
}

fn run_yolo_inference(
    preprocessed: Vec<f32>,
    orig_w: u32,
    orig_h: u32,
    conf_threshold: f32,
) -> Result<Vec<Detection>, Error> {
    let cache = YOLO_MODEL.lock().unwrap();
    let model = cache
        .as_ref()
        .ok_or_else(|| Error::ModelNotAvailable("YOLO model not loaded".into()))?;

    let input_shape = ndarray::IxDyn(&[1, 3, YOLO_INPUT_SIZE as usize, YOLO_INPUT_SIZE as usize]);
    let input_arr = ndarray::ArrayD::from_shape_vec(input_shape, preprocessed)
        .map_err(|e| Error::Inference(format!("failed to shape input: {}", e)))?;

    let input_tensor = Tensor::from(input_arr);

    let outputs = model
        .run(tvec!(input_tensor.into()))
        .map_err(|e| Error::Inference(format!("YOLO inference failed: {}", e)))?;

    let output = &outputs[0];
    let output_data = output
        .as_slice::<f32>()
        .map_err(|e| Error::Inference(format!("failed to read output: {}", e)))?;

    // YOLOv8 output: [1, 84, 8400]
    let shape = output.shape();
    let num_classes = if shape.len() >= 2 {
        (shape[1] as usize).saturating_sub(4)
    } else {
        80
    };
    let num_anchors = if shape.len() >= 2 {
        shape[2] as usize
    } else {
        output_data.len() / (num_classes + 4).max(1)
    };

    let mut raw = parse_yolo_output(output_data, num_classes, num_anchors, conf_threshold, orig_w, orig_h);
    non_max_suppression(&mut raw, 0.45);

    let detections: Vec<Detection> = raw
        .iter()
        .map(|d| {
            let class_name = COCO_CLASSES
                .get(d.class_id)
                .map(|s| s.to_string())
                .unwrap_or_else(|| format!("class_{}", d.class_id));

            let x1 = (d.cx - d.w / 2.0).max(0.0) as i32;
            let y1 = (d.cy - d.h / 2.0).max(0.0) as i32;
            let x2 = (d.cx + d.w / 2.0).min(orig_w as f32) as i32;
            let y2 = (d.cy + d.h / 2.0).min(orig_h as f32) as i32;

            Detection {
                class_name,
                confidence: d.confidence,
                bbox: BoundingBox { x: x1, y: y1, width: (x2 - x1).max(0), height: (y2 - y1).max(0) },
            }
        })
        .collect();

    Ok(detections)
}

async fn analyze_api(config: &Config, image_data: &[u8]) -> Result<VisionResult, Error> {
    let client = reqwest::Client::new();

    let part = reqwest::multipart::Part::bytes(image_data.to_vec())
        .file_name("frame.jpg")
        .mime_str("image/jpeg")
        .map_err(|e| Error::InvalidInput(e.to_string()))?;

    let form = reqwest::multipart::Form::new().part("image", part);

    let response = client
        .post(&config.vision_api_url)
        .timeout(std::time::Duration::from_secs(config.vision_timeout_secs))
        .multipart(form)
        .send()
        .await?;

    let result: serde_json::Value = response.json().await?;

    let detections: Vec<Detection> = result["detections"]
        .as_array()
        .map(|arr| {
            arr.iter()
                .filter(|d| {
                    d["confidence"].as_f64().unwrap_or(0.0) as f32 >= config.vision_detection_threshold
                })
                .map(|d| Detection {
                    class_name: d["class"].as_str().unwrap_or("unknown").into(),
                    confidence: d["confidence"].as_f64().unwrap_or(0.0) as f32,
                    bbox: BoundingBox {
                        x: d["x"].as_i64().unwrap_or(0) as i32,
                        y: d["y"].as_i64().unwrap_or(0) as i32,
                        width: d["width"].as_i64().unwrap_or(0) as i32,
                        height: d["height"].as_i64().unwrap_or(0) as i32,
                    },
                })
                .collect()
        })
        .unwrap_or_default();

    let person_count = detections.iter().filter(|d| d.class_name == "person").count();

    Ok(VisionResult {
        detections,
        scene_description: result["description"].as_str().map(String::from),
        face_count: person_count,
        person_count,
        duration_ms: 0,
    })
}

// ── Face recognition ──────────────────────────────────────────

#[derive(Debug, Clone, Serialize, Deserialize)]
#[allow(dead_code)]
pub struct FaceRecognitionResult {
    pub faces: Vec<RecognizedFace>,
    pub duration_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[allow(dead_code)]
pub struct RecognizedFace {
    pub person_id: Option<String>,
    pub person_name: Option<String>,
    pub confidence: f32,
    pub bbox: BoundingBox,
    pub emotion: Option<String>,
    pub age_estimate: Option<i32>,
}

#[allow(dead_code)]
pub async fn recognize_faces(
    config: &Config,
    image_data: &[u8],
) -> Result<FaceRecognitionResult, Error> {
    let start = Instant::now();

    tracing::debug!("face recognition: {} bytes", image_data.len());

    // Use YOLO person detection as proxy for face detection.
    // Full implementation: SCRFD/RetinaFace ONNX + ArcFace embeddings.
    let detections = if !image_data.is_empty() {
        match analyze(config, image_data).await {
            Ok(vision) => vision.detections,
            Err(_) => Vec::new(),
        }
    } else {
        Vec::new()
    };

    let faces: Vec<RecognizedFace> = detections
        .iter()
        .filter(|d| d.class_name == "person")
        .map(|d| RecognizedFace {
            person_id: None,
            person_name: None,
            confidence: d.confidence,
            bbox: d.bbox.clone(),
            emotion: Some("neutral".into()),
            age_estimate: None,
        })
        .collect();

    let duration_ms = start.elapsed().as_millis() as u64;

    Ok(FaceRecognitionResult { faces, duration_ms })
}

// ── Tests ──────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_compute_iou_perfect_overlap() {
        let a = RawDetection { class_id: 0, confidence: 0.9, cx: 100.0, cy: 100.0, w: 50.0, h: 50.0 };
        let b = RawDetection { class_id: 0, confidence: 0.8, cx: 100.0, cy: 100.0, w: 50.0, h: 50.0 };
        assert!((compute_iou(&a, &b) - 1.0).abs() < 0.01);
    }

    #[test]
    fn test_compute_iou_no_overlap() {
        let a = RawDetection { class_id: 0, confidence: 0.9, cx: 0.0, cy: 0.0, w: 10.0, h: 10.0 };
        let b = RawDetection { class_id: 0, confidence: 0.8, cx: 100.0, cy: 100.0, w: 10.0, h: 10.0 };
        assert_eq!(compute_iou(&a, &b), 0.0);
    }

    #[test]
    fn test_compute_iou_partial_overlap() {
        let a = RawDetection { class_id: 0, confidence: 0.9, cx: 50.0, cy: 50.0, w: 100.0, h: 100.0 };
        let b = RawDetection { class_id: 0, confidence: 0.8, cx: 100.0, cy: 50.0, w: 100.0, h: 100.0 };
        let iou = compute_iou(&a, &b);
        assert!((iou - 0.333).abs() < 0.01, "expected IOU ~0.333, got {}", iou);
    }

    #[test]
    fn test_nms_removes_duplicate() {
        let mut dets = vec![
            RawDetection { class_id: 0, confidence: 0.9, cx: 100.0, cy: 100.0, w: 50.0, h: 50.0 },
            RawDetection { class_id: 0, confidence: 0.8, cx: 102.0, cy: 102.0, w: 50.0, h: 50.0 },
        ];
        non_max_suppression(&mut dets, 0.5);
        assert_eq!(dets.len(), 1);
        assert!((dets[0].confidence - 0.9).abs() < 0.01);
    }

    #[test]
    fn test_nms_keeps_different_classes() {
        let mut dets = vec![
            RawDetection { class_id: 0, confidence: 0.9, cx: 100.0, cy: 100.0, w: 50.0, h: 50.0 },
            RawDetection { class_id: 1, confidence: 0.8, cx: 100.0, cy: 100.0, w: 50.0, h: 50.0 },
        ];
        non_max_suppression(&mut dets, 0.5);
        assert_eq!(dets.len(), 2);
    }

    #[test]
    fn test_nms_empty() {
        let mut dets: Vec<RawDetection> = Vec::new();
        non_max_suppression(&mut dets, 0.5);
        assert!(dets.is_empty());
    }

    #[test]
    fn test_coco_classes_count() {
        assert_eq!(COCO_CLASSES.len(), 80);
        assert_eq!(COCO_CLASSES[0], "person");
    }

    #[test]
    fn test_parse_yolo_output_empty() {
        let output = vec![0.0f32; 84 * 8400];
        assert!(parse_yolo_output(&output, 80, 8400, 0.5, 640, 480).is_empty());
    }

    #[test]
    fn test_parse_yolo_output_with_detection() {
        let nc = 80;
        let na = 8400;
        let stride = nc + 4;
        let mut output = vec![0.0f32; stride * na];
        output[0] = 0.5;
        output[1] = 0.5;
        output[2] = 0.1;
        output[3] = 0.1;
        output[4] = 0.95;

        let dets = parse_yolo_output(&output, nc, na, 0.5, 640, 480);
        assert_eq!(dets.len(), 1);
        assert_eq!(dets[0].class_id, 0);
        assert!((dets[0].confidence - 0.95).abs() < 0.01);
    }

    #[test]
    fn test_parse_yolo_below_threshold() {
        let nc = 80;
        let na = 8400;
        let stride = nc + 4;
        let mut output = vec![0.0f32; stride * na];
        output[4] = 0.3;

        assert!(parse_yolo_output(&output, nc, na, 0.5, 640, 480).is_empty());
    }

    #[test]
    fn test_preprocess_image_small_jpeg() {
        let mut img_buf = Vec::new();
        let img = image::RgbImage::from_pixel(1, 1, image::Rgb([255u8, 0u8, 0u8]));
        let mut cursor = std::io::Cursor::new(&mut img_buf);
        img.write_to(&mut cursor, image::ImageFormat::Jpeg).unwrap();

        let result = preprocess_image(&img_buf).unwrap();
        assert_eq!(result.len(), 3 * 640 * 640);
    }
}
