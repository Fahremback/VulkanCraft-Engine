#!/usr/bin/env node
// authoring-jobs.mjs — Generic async job manager for long authoring
// operations (import, cook, package, server, run, ...). Same contract as the
// build-job system (start_build/build_status/cancel_build/list_build_jobs)
// already in server.mjs, but domain-agnostic: any long-running task can be
// wrapped as a job with queued -> running -> succeeded|failed|cancelled, plus
// retry. The task receives a cancellation signal so process-based jobs can
// kill their child on cancel (never leaving a window/process open).
//
// States (single source of truth, no ALL-PASSED-style hardcoded claims):
//   queued -> running -> succeeded | failed | cancelled
// cancel() only transitions queued/running; retry() only failed/cancelled
// (attempts < max_attempts, default 3).

export function createJobManager({ onEvent = () => {} } = {}) {
  const jobs = new Map();   // jobId -> job record
  let nextJobId = 1;
  const MAX_ATTEMPTS = 3;

  function snapshot(job) {
    return {
      job_id: job.job_id,
      kind: job.kind,
      status: job.status,
      payload: job.payload,
      attempts: job.attempts,
      max_attempts: MAX_ATTEMPTS,
      started_at: job.started_at,
      finished_at: job.finished_at ?? null,
      exit_code: job.exit_code ?? null,
      error: job.error ?? null,
      result: job.result ?? null
    };
  }

  function signal() {
    return {
      aborted: false,
      listeners: new Set(),
      onAbort(fn) { this.listeners.add(fn); },
      abort() {
        if (this.aborted) return;
        this.aborted = true;
        for (const fn of this.listeners) { try { fn(); } catch { /* listener must not kill the manager */ } }
        this.listeners.clear();
      }
    };
  }

  function run(job) {
    job.status = "running";
    job.started_at = new Date().toISOString();
    job.signal = signal();
    const abort = job.signal;
    Promise.resolve()
      .then(() => job.task(abort))
      .then((result) => {
        if (abort.aborted) return;   // cancel wins over a late result
        job.status = "succeeded";
        job.result = result;
        job.finished_at = new Date().toISOString();
        onEvent(job);
      })
      .catch((error) => {
        if (abort.aborted) return;
        job.status = "failed";
        job.error = error instanceof Error ? error.message : String(error);
        job.finished_at = new Date().toISOString();
        onEvent(job);
      });
  }

  return {
    start(kind, payload, task) {
      if (typeof task !== "function") throw new Error("job task must be a function");
      const job = {
        job_id: nextJobId++,
        kind,
        payload,
        task,
        status: "queued",
        attempts: 1,
        started_at: null,
        finished_at: null,
        exit_code: null,
        error: null,
        result: null,
        signal: null
      };
      jobs.set(job.job_id, job);
      setImmediate(() => run(job));
      return snapshot(job);
    },

    status(jobId) {
      const job = jobs.get(Number(jobId));
      if (!job) throw new Error(`unknown job '${jobId}' (see list_jobs)`);
      return snapshot(job);
    },

    cancel(jobId) {
      const job = jobs.get(Number(jobId));
      if (!job) throw new Error(`unknown job '${jobId}' (see list_jobs)`);
      if (job.status !== "queued" && job.status !== "running") {
        return { job_id: job.job_id, cancelled: false, status: job.status };
      }
      job.status = "cancelled";
      job.finished_at = new Date().toISOString();
      job.signal?.abort();
      onEvent(job);
      return { job_id: job.job_id, cancelled: true, status: "cancelled" };
    },

    retry(jobId) {
      const job = jobs.get(Number(jobId));
      if (!job) throw new Error(`unknown job '${jobId}' (see list_jobs)`);
      if (job.status === "running" || job.status === "queued") {
        return { job_id: job.job_id, retried: false, status: job.status };
      }
      if (job.attempts >= MAX_ATTEMPTS) {
        return { job_id: job.job_id, retried: false, status: job.status, reason: `max attempts (${MAX_ATTEMPTS}) reached` };
      }
      job.status = "queued";
      job.attempts += 1;
      job.error = null;
      job.result = null;
      job.exit_code = null;
      job.signal = null;
      setImmediate(() => run(job));
      return { job_id: job.job_id, retried: true, status: "queued", attempts: job.attempts };
    },

    list() {
      return [...jobs.values()]
        .sort((a, b) => b.job_id - a.job_id)
        .map(snapshot);
    }
  };
}
