/// Prometheus metrics for the Device Manager.
use lazy_static::lazy_static;
use prometheus::{register_int_counter, register_int_gauge, IntCounter, IntGauge};

lazy_static! {
    pub static ref ONLINE_ROBOTS: IntGauge = register_int_gauge!(
        "device_mgr_online_robots",
        "Number of robots currently online"
    )
    .unwrap();

    pub static ref ROBOT_OFFLINE_EVENTS: IntCounter = register_int_counter!(
        "device_mgr_offline_events_total",
        "Total number of robot offline events detected"
    )
    .unwrap();

    pub static ref HEARTBEAT_COUNT: IntCounter = register_int_counter!(
        "device_mgr_heartbeats_total",
        "Total heartbeat messages received"
    )
    .unwrap();

    pub static ref SENSOR_DATA_COUNT: IntCounter = register_int_counter!(
        "device_mgr_sensor_data_total",
        "Total sensor data points stored"
    )
    .unwrap();

    pub static ref REGISTRATIONS: IntCounter = register_int_counter!(
        "device_mgr_registrations_total",
        "Total robot registrations"
    )
    .unwrap();
}
