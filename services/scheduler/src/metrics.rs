/// Prometheus metrics for the Scheduler.
use lazy_static::lazy_static;
use prometheus::{register_int_counter, IntCounter};

lazy_static! {
    pub static ref TASKS_CREATED: IntCounter = register_int_counter!(
        "scheduler_tasks_created_total",
        "Total tasks created"
    )
    .unwrap();

    pub static ref TASKS_ASSIGNED: IntCounter = register_int_counter!(
        "scheduler_tasks_assigned_total",
        "Total tasks assigned to robots"
    )
    .unwrap();

    pub static ref TASKS_DISPATCHED: IntCounter = register_int_counter!(
        "scheduler_tasks_dispatched_total",
        "Total tasks dispatched via NATS"
    )
    .unwrap();

    pub static ref TASKS_COMPLETED: IntCounter = register_int_counter!(
        "scheduler_tasks_completed_total",
        "Total tasks completed successfully"
    )
    .unwrap();

    pub static ref TASKS_FAILED: IntCounter = register_int_counter!(
        "scheduler_tasks_failed_total",
        "Total tasks failed (including retries exhausted)"
    )
    .unwrap();
}
