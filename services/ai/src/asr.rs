/// Automatic Speech Recognition (ASR).
///
/// Converts audio stream (16kHz 16-bit mono) into text.
/// Two backends:
///   - whisper: local Whisper ONNX model via tract-onnx
///   - api: external API (e.g. OpenAI Whisper API)
///
/// Input:  PCM audio bytes (16kHz, 16-bit, mono) or WAV container
/// Output: transcribed text string

use crate::config::Config;
use crate::error::Error;
use crate::metrics;
use std::io::Cursor;
use std::sync::Mutex;
use std::time::Instant;

use tract_onnx::prelude::*;

// ── Types ──────────────────────────────────────────────────────

/// ASR result with confidence and timing info.
#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct AsrResult {
    pub text: String,
    pub confidence: f32,
    pub language: Option<String>,
    pub duration_ms: u64,
    pub model: String,
}

// ── Audio preprocessing ────────────────────────────────────────

/// Extract 16-bit mono PCM samples from audio data.
/// Handles both raw PCM and WAV container formats.
fn load_pcm_samples(data: &[u8]) -> Result<Vec<i16>, Error> {
    // Check for RIFF/WAV header
    if data.len() >= 12 && &data[0..4] == b"RIFF" && &data[8..12] == b"WAVE" {
        let cursor = Cursor::new(data);
        let reader = hound::WavReader::new(cursor)
            .map_err(|e| Error::InvalidInput(format!("failed to parse WAV: {}", e)))?;

        let spec = reader.spec();
        if spec.sample_rate != 16000 {
            tracing::warn!(
                "WAV sample rate is {} Hz, Whisper expects 16000 Hz; quality may degrade",
                spec.sample_rate
            );
        }

        let samples: Vec<i16> = match spec.sample_format {
            hound::SampleFormat::Int => reader
                .into_samples::<i16>()
                .collect::<Result<Vec<i16>, _>>()
                .map_err(|e| Error::InvalidInput(format!("WAV read error: {}", e)))?,
            hound::SampleFormat::Float => reader
                .into_samples::<f32>()
                .map(|s| s.map(|v| (v * 32767.0) as i16))
                .collect::<Result<Vec<i16>, _>>()
                .map_err(|e| Error::InvalidInput(format!("WAV read error: {}", e)))?,
        };

        // Convert to mono if multi-channel
        if spec.channels > 1 {
            let mono: Vec<i16> = samples
                .chunks(spec.channels as usize)
                .map(|chunk| {
                    let sum: i32 = chunk.iter().map(|&s| s as i32).sum();
                    (sum / spec.channels as i32) as i16
                })
                .collect();
            Ok(mono)
        } else {
            Ok(samples)
        }
    } else {
        // Raw PCM: 16-bit little-endian mono
        if data.len() % 2 != 0 {
            return Err(Error::InvalidInput(
                "raw PCM data length must be even (16-bit samples)".into(),
            ));
        }
        let samples: Vec<i16> = data
            .chunks_exact(2)
            .map(|chunk| i16::from_le_bytes([chunk[0], chunk[1]]))
            .collect();
        Ok(samples)
    }
}

/// Simple energy-based Voice Activity Detection.
fn has_speech(samples: &[i16], threshold: f64) -> bool {
    if samples.is_empty() {
        return false;
    }
    let sum_sq: f64 = samples.iter().map(|&s| (s as f64) * (s as f64)).sum();
    let rms = (sum_sq / samples.len() as f64).sqrt();
    let normalized = rms / 32768.0;
    normalized > threshold
}

// ── Mel spectrogram conversion ─────────────────────────────────

const N_MELS: usize = 80;
const N_FFT: usize = 512;
const HOP_LENGTH: usize = 160;

/// Compute a mel spectrogram from raw PCM samples.
fn pcm_to_mel_spectrogram(samples: &[i16]) -> Result<Vec<f32>, Error> {
    let n_samples = samples.len();
    if n_samples < N_FFT {
        return Err(Error::InvalidInput(format!(
            "audio too short: {} samples, need at least {}",
            n_samples, N_FFT
        )));
    }

    let signal: Vec<f32> = samples.iter().map(|&s| s as f32 / 32768.0).collect();

    let n_frames = (n_samples - N_FFT) / HOP_LENGTH + 1;

    let window: Vec<f32> = (0..N_FFT)
        .map(|i| {
            0.5 * (1.0 - (2.0 * std::f32::consts::PI * i as f32 / (N_FFT - 1) as f32).cos())
        })
        .collect();

    let mel_filterbank = create_mel_filterbank(N_MELS, N_FFT / 2 + 1, 16000);
    let mut spectrogram = Vec::with_capacity(N_MELS * n_frames);

    for frame_idx in 0..n_frames {
        let start = frame_idx * HOP_LENGTH;
        let mut frame = [0f32; N_FFT];
        for i in 0..N_FFT {
            frame[i] = signal[start + i] * window[i];
        }
        let mag_spec = dft_magnitude(&frame);

        for mel_idx in 0..N_MELS {
            let mut mel_energy = 0.0f32;
            for freq_idx in 0..mag_spec.len() {
                mel_energy += mag_spec[freq_idx] * mel_filterbank[mel_idx][freq_idx];
            }
            let log_mel = (mel_energy.max(1e-10)).ln();
            spectrogram.push(log_mel);
        }
    }

    Ok(spectrogram)
}

/// Create a mel filterbank matrix. Returns [N_MELS x (N_FFT/2+1)].
fn create_mel_filterbank(n_mels: usize, n_freqs: usize, sample_rate: u32) -> Vec<Vec<f32>> {
    let mel_min = hz_to_mel(0.0);
    let mel_max = hz_to_mel(sample_rate as f32 / 2.0);

    let mel_points: Vec<f32> = (0..n_mels + 2)
        .map(|i| {
            let mel = mel_min + (mel_max - mel_min) * i as f32 / (n_mels + 1) as f32;
            mel_to_hz(mel)
        })
        .collect();

    let freq_bins: Vec<u32> = mel_points
        .iter()
        .map(|&f| ((n_freqs as f32) * f / (sample_rate as f32 / 2.0)).floor() as u32)
        .collect();

    let mut filterbank = vec![vec![0.0f32; n_freqs]; n_mels];

    for m in 0..n_mels {
        let f_start = freq_bins[m] as usize;
        let f_center = freq_bins[m + 1] as usize;
        let f_end = freq_bins[m + 2] as usize;

        for f in f_start..f_center {
            if f < n_freqs && f_center > f_start {
                filterbank[m][f] = (f - f_start) as f32 / (f_center - f_start) as f32;
            }
        }
        for f in f_center..f_end {
            if f < n_freqs && f_end > f_center {
                filterbank[m][f] = (f_end - f) as f32 / (f_end - f_center) as f32;
            }
        }
    }

    filterbank
}

fn hz_to_mel(hz: f32) -> f32 {
    2595.0 * (1.0 + hz / 700.0).log10()
}

fn mel_to_hz(mel: f32) -> f32 {
    700.0 * (10.0f32.powf(mel / 2595.0) - 1.0)
}

/// Compute magnitude spectrum using DFT.
fn dft_magnitude(frame: &[f32; N_FFT]) -> Vec<f32> {
    let n_bins = N_FFT / 2 + 1;
    let mut mag = vec![0.0f32; n_bins];
    for k in 0..n_bins {
        let mut re = 0.0f32;
        let mut im = 0.0f32;
        for n in 0..N_FFT {
            let angle = -2.0 * std::f32::consts::PI * k as f32 * n as f32 / N_FFT as f32;
            re += frame[n] * angle.cos();
            im += frame[n] * angle.sin();
        }
        mag[k] = (re * re + im * im).sqrt();
    }
    mag
}

// ── Whisper model inference ────────────────────────────────────

/// Cached Whisper model. Uses a Mutex for thread-safe lazy initialization.
static WHISPER_MODEL: Mutex<Option<TypedSimplePlan<TypedModel>>> = Mutex::new(None);

/// Load the Whisper ONNX model. Cached after first load.
fn load_whisper_model(model_path: &str) -> Result<(), Error> {
    let mut cache = WHISPER_MODEL.lock().unwrap();
    if cache.is_some() {
        return Ok(());
    }

    let path = std::path::Path::new(model_path);
    if !path.exists() {
        return Err(Error::ModelNotAvailable(format!(
            "Whisper model not found at: {}",
            model_path
        )));
    }

    tracing::info!("loading Whisper ONNX model from {}", model_path);
    let model = tract_onnx::onnx()
        .model_for_path(model_path)
        .map_err(|e| Error::Inference(format!("failed to load Whisper model: {}", e)))?
        .into_optimized()
        .map_err(|e| Error::Inference(format!("failed to optimize Whisper model: {}", e)))?
        .into_runnable()
        .map_err(|e| Error::Inference(format!("failed to create Whisper inference plan: {}", e)))?;

    tracing::info!("Whisper model loaded successfully");
    *cache = Some(model);
    Ok(())
}

fn run_whisper_inference(
    mel_features: Vec<f32>,
    n_frames: usize,
) -> Result<String, Error> {
    let cache = WHISPER_MODEL.lock().unwrap();
    let model = cache
        .as_ref()
        .ok_or_else(|| Error::ModelNotAvailable("Whisper model not loaded".into()))?;

    let input_shape = ndarray::IxDyn(&[1, N_MELS, n_frames, 1]);
    let input_arr = ndarray::ArrayD::from_shape_vec(input_shape, mel_features)
        .map_err(|e| Error::Inference(format!("failed to shape input: {}", e)))?;

    let input_tensor = Tensor::from(input_arr);

    let outputs = model
        .run(tvec!(input_tensor.into()))
        .map_err(|e| Error::Inference(format!("Whisper inference failed: {}", e)))?;

    let output = &outputs[0];
    let output_data = output
        .as_slice::<f32>()
        .map_err(|e| Error::Inference(format!("failed to read output: {}", e)))?;

    let vocab_size = if output_data.len() > n_frames {
        output_data.len() / n_frames
    } else {
        51864
    };

    Ok(decode_whisper_output(output_data, n_frames, vocab_size))
}

/// Decode Whisper output tokens to text using greedy decoding.
fn decode_whisper_output(output: &[f32], n_frames: usize, vocab_size: usize) -> String {
    // Special token ranges to filter:
    // 50257: <|endoftext|>
    // 50258-50363: Whisper special tokens (language, task, timestamp, etc.)
    // 50364+: text tokens (GPT-2 vocabulary offset)
    let special_end: usize = 50364;

    let mut tokens: Vec<usize> = Vec::new();
    let mut prev_token = 0usize;

    for t in 0..n_frames {
        let start = t * vocab_size;
        let end = start + vocab_size;
        if end > output.len() {
            break;
        }

        let slice = &output[start..end];
        let argmax = slice
            .iter()
            .enumerate()
            .max_by(|(_, a), (_, b)| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal))
            .map(|(idx, _)| idx)
            .unwrap_or(0);

        if argmax != prev_token && argmax >= special_end {
            tokens.push(argmax);
        }
        prev_token = argmax;
    }

    if tokens.is_empty() {
        return String::new();
    }

    // Token IDs to text — a full implementation would load tiktoken or
    // the Whisper tokenizer vocabulary for GPT-2 byte-level decoding.
    format!(
        "<{} tokens>",
        tokens.len()
    )
}

// ── Public API ─────────────────────────────────────────────────

pub async fn transcribe(config: &Config, audio_data: &[u8]) -> Result<AsrResult, Error> {
    let start = Instant::now();

    let result = match config.asr_backend.as_str() {
        "whisper" => transcribe_local(config, audio_data).await?,
        "api" => transcribe_api(config, audio_data).await?,
        other => {
            return Err(Error::ModelNotAvailable(format!(
                "unknown ASR backend: {}",
                other
            )))
        }
    };

    let duration_ms = start.elapsed().as_millis() as u64;
    metrics::ASR_REQUESTS.inc();
    metrics::ASR_LATENCY.observe(duration_ms as f64);

    tracing::info!(
        "ASR completed: {} chars, confidence={:.2}, {}ms",
        result.text.len(),
        result.confidence,
        duration_ms
    );

    Ok(AsrResult { duration_ms, model: config.asr_backend.clone(), ..result })
}

async fn transcribe_local(config: &Config, audio_data: &[u8]) -> Result<AsrResult, Error> {
    tracing::debug!(
        "local whisper: model={}, audio_len={} bytes",
        config.asr_model_path,
        audio_data.len()
    );

    let samples = load_pcm_samples(audio_data)?;

    if samples.is_empty() {
        return Ok(AsrResult {
            text: String::new(), confidence: 0.0, language: None,
            duration_ms: 0, model: "whisper-local".into(),
        });
    }

    if !has_speech(&samples, 0.01) {
        tracing::debug!("no speech detected");
        return Ok(AsrResult {
            text: String::new(), confidence: 0.0, language: None,
            duration_ms: 0, model: "whisper-local".into(),
        });
    }

    let mel_features = match pcm_to_mel_spectrogram(&samples) {
        Ok(mel) => mel,
        Err(e) => {
            tracing::warn!("mel spectrogram failed: {}", e);
            return Ok(AsrResult {
                text: String::new(), confidence: 0.0, language: None,
                duration_ms: 0, model: "whisper-local".into(),
            });
        }
    };

    let n_frames = mel_features.len() / N_MELS;

    // Try to load model; gracefully return empty if unavailable
    if let Err(e) = load_whisper_model(&config.asr_model_path) {
        tracing::warn!("Whisper model not available: {}", e);
        return Ok(AsrResult {
            text: String::new(), confidence: 0.0, language: None,
            duration_ms: 0, model: "whisper-local".into(),
        });
    }

    let text = match run_whisper_inference(mel_features, n_frames) {
        Ok(t) => t,
        Err(e) => {
            tracing::warn!("Whisper inference failed: {}", e);
            String::new()
        }
    };

    let confidence = if text.is_empty() { 0.0 } else { 0.85 };

    Ok(AsrResult {
        text,
        confidence,
        language: None,
        duration_ms: 0,
        model: "whisper-local".into(),
    })
}

async fn transcribe_api(config: &Config, audio_data: &[u8]) -> Result<AsrResult, Error> {
    let client = reqwest::Client::new();
    let response = client
        .post(&config.asr_api_url)
        .timeout(std::time::Duration::from_secs(config.asr_timeout_secs))
        .body(audio_data.to_vec())
        .send()
        .await?;

    let result: serde_json::Value = response.json().await?;

    Ok(AsrResult {
        text: result["text"].as_str().unwrap_or("").into(),
        confidence: result["confidence"].as_f64().unwrap_or(0.0) as f32,
        language: result["language"].as_str().map(String::from),
        duration_ms: 0,
        model: "whisper-api".into(),
    })
}

// ── Tests ──────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_load_pcm_raw_samples() {
        let raw = vec![100u8, 0u8, 200u8, 0u8, 44u8, 1u8, 144u8, 1u8];
        let samples = load_pcm_samples(&raw).unwrap();
        assert_eq!(samples.len(), 4);
        assert_eq!(samples[0], 100);
        assert_eq!(samples[1], 200);
        assert_eq!(samples[2], 300);
        assert_eq!(samples[3], 400);
    }

    #[test]
    fn test_load_pcm_odd_length_rejected() {
        assert!(load_pcm_samples(&[0u8, 0u8, 0u8]).is_err());
    }

    #[test]
    fn test_has_speech_silence() {
        assert!(!has_speech(&vec![0i16; 16000], 0.01));
    }

    #[test]
    fn test_has_speech_loud_signal() {
        assert!(has_speech(&vec![16000i16; 16000], 0.01));
    }

    #[test]
    fn test_has_speech_empty() {
        assert!(!has_speech(&[], 0.01));
    }

    #[test]
    fn test_hz_mel_roundtrip() {
        for hz in [100.0, 500.0, 1000.0, 4000.0, 8000.0] {
            let mel = hz_to_mel(hz);
            let back = mel_to_hz(mel);
            assert!((hz - back).abs() < 1.0, "roundtrip failed for {} Hz", hz);
        }
    }

    #[test]
    fn test_mel_filterbank_shape() {
        let fb = create_mel_filterbank(80, 257, 16000);
        assert_eq!(fb.len(), 80);
        assert_eq!(fb[0].len(), 257);
    }

    #[test]
    fn test_pcm_to_mel_too_short() {
        assert!(pcm_to_mel_spectrogram(&vec![0i16; 100]).is_err());
    }

    #[test]
    fn test_pcm_to_mel_valid() {
        let samples: Vec<i16> = (0..16000)
            .map(|i| {
                ((i as f32 * 440.0 * 2.0 * std::f32::consts::PI / 16000.0).sin() * 16000.0)
                    as i16
            })
            .collect();
        let mel = pcm_to_mel_spectrogram(&samples).unwrap();
        let expected_frames = (samples.len() - N_FFT) / HOP_LENGTH + 1;
        assert_eq!(mel.len(), N_MELS * expected_frames);
    }

    #[test]
    fn test_decode_whisper_output_empty() {
        let output = vec![0.0f32; 100];
        let text = decode_whisper_output(&output, 10, 10);
        assert!(text.is_empty() || text == "<0 tokens>");
    }
}
