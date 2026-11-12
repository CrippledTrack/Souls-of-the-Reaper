//! The one job the Build panel runs at a time: a build or a tool install.

use crate::message::{msg, Message};
use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc, Mutex, MutexGuard, PoisonError,
};

/// What a cancelled build returns.
pub fn cancelled() -> Message {
    msg!("err.build.cancelled")
}

/// What the Build panel runs. There is one at a time: start_build and
/// install_tools claim the slot before their first check, so a second click
/// is turned away before it fetches or starts anything.
#[derive(Debug, Default)]
pub enum Job {
    #[default]
    Idle,
    /// A build: checking tools and fetching sources until `pid` is set, then
    /// scripts/build_client.py. Cancel sets `cancel`, which also stops a fetch
    /// and keeps the script from starting.
    Build { cancel: Arc<AtomicBool>, pid: Option<u32> },
    /// winget and the installers it starts, which are not stopped halfway.
    Install,
}

#[derive(Debug, Default)]
pub struct JobSlot(Mutex<Job>);

/// A claimed slot. Dropping it frees the slot, so every exit path of the
/// command that holds it (errors and `?` included) does.
pub struct Claim<'a> {
    slot: &'a JobSlot,
    pub(crate) cancel: Arc<AtomicBool>,
}

impl JobSlot {
    pub(crate) fn lock(&self) -> MutexGuard<'_, Job> {
        self.0.lock().unwrap_or_else(PoisonError::into_inner)
    }

    /// Takes the slot for a build, or for a tool install when `build` is false.
    pub(crate) fn claim(&self, build: bool) -> Result<Claim<'_>, Message> {
        let mut job = self.lock();
        match *job {
            Job::Idle => {}
            Job::Build { .. } => return Err(msg!("err.build.running")),
            Job::Install => return Err(msg!("err.build.installing")),
        }
        let cancel = Arc::new(AtomicBool::new(false));
        *job = if build {
            Job::Build { cancel: cancel.clone(), pid: None }
        } else {
            Job::Install
        };
        Ok(Claim { slot: self, cancel })
    }

    /// Cancels a build and hands over its script's process id once it runs,
    /// for the caller to stop (only once, so a second Cancel cannot hit a
    /// reused id). Tool installs are left to finish.
    pub(crate) fn cancel(&self) -> Option<u32> {
        match &mut *self.lock() {
            Job::Build { cancel, pid } => {
                cancel.store(true, Ordering::SeqCst);
                pid.take()
            }
            _ => None,
        }
    }
}

impl Claim<'_> {
    pub(crate) fn cancelled(&self) -> bool {
        self.cancel.load(Ordering::SeqCst)
    }

    /// Records the started build script for Cancel. False when Cancel came
    /// first; the caller then stops the script itself.
    pub(crate) fn started(&self, id: u32) -> bool {
        let mut job = self.slot.lock();
        if self.cancelled() {
            return false;
        }
        if let Job::Build { pid, .. } = &mut *job {
            *pid = Some(id);
        }
        true
    }
}

impl Drop for Claim<'_> {
    fn drop(&mut self) {
        *self.slot.lock() = Job::Idle;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn one_job_runs_at_a_time_until_its_claim_drops() {
        let slot = JobSlot::default();
        let build = slot.claim(true).unwrap();
        assert_eq!(slot.claim(true).err().map(|e| e.key), Some("err.build.running"));
        assert!(slot.claim(false).is_err(), "no install during a build");
        drop(build);
        let install = slot.claim(false).unwrap();
        assert_eq!(slot.claim(false).err().map(|e| e.key), Some("err.build.installing"));
        assert!(slot.claim(true).is_err(), "no build during an install");
        drop(install);
        assert!(slot.claim(true).is_ok());
    }

    #[test]
    fn simultaneous_build_clicks_admit_one() {
        let slot = JobSlot::default();
        let start = std::sync::Barrier::new(8);
        let admitted = std::thread::scope(|s| {
            let tries: Vec<_> = (0..8)
                .map(|_| {
                    s.spawn(|| {
                        start.wait();
                        // Kept, not dropped, so the slot stays taken for the others.
                        slot.claim(true).map(std::mem::forget).is_ok()
                    })
                })
                .collect();
            tries.into_iter().map(|t| t.join().unwrap()).filter(|&ok| ok).count()
        });
        assert_eq!(admitted, 1);
    }

    #[test]
    fn cancel_while_preparing_keeps_the_script_from_starting() {
        let slot = JobSlot::default();
        let claim = slot.claim(true).unwrap();
        assert!(!claim.cancelled());
        assert_eq!(slot.cancel(), None, "no process to stop yet");
        assert!(claim.cancelled(), "the fetch sees the flag");
        assert!(!claim.started(42), "a script started after Cancel is refused");
        assert_eq!(slot.cancel(), None, "and not recorded");
    }

    #[test]
    fn cancel_stops_a_running_script() {
        let slot = JobSlot::default();
        let claim = slot.claim(true).unwrap();
        assert!(claim.started(42));
        assert_eq!(slot.cancel(), Some(42));
        assert!(claim.cancelled());
        assert_eq!(slot.cancel(), None, "a second Cancel stops nothing again");
        drop(claim);
        assert!(slot.claim(true).is_ok(), "the slot is free once the build returns");
    }

    #[test]
    fn tool_installs_are_not_cancelled() {
        let slot = JobSlot::default();
        let install = slot.claim(false).unwrap();
        assert_eq!(slot.cancel(), None);
        assert!(!install.cancelled());
    }
}
