/**
 * Small retry helper with exponential backoff + jitter, honoring an explicit
 * retry-after hint (as cloud APIs send on HTTP 429). Kept dependency-free.
 */

export interface RetryableError extends Error {
  /** Seconds to wait before retrying, if the server told us (e.g. Retry-After). */
  retryAfterSeconds?: number;
  /** Set false to make an error non-retryable regardless of attempt count. */
  retryable?: boolean;
}

export interface RetryOptions {
  retries: number;
  baseDelayMs: number;
  maxDelayMs: number;
  /** Decide whether a thrown error is worth retrying. Default: err.retryable !== false. */
  isRetryable?: (error: unknown) => boolean;
  /** Injectable sleeper (tests pass a no-op). Default: real setTimeout. */
  sleep?: (ms: number) => Promise<void>;
  /** Injectable jitter in [0,1). Default: Math.random. Tests pass () => 0. */
  random?: () => number;
  onRetry?: (attempt: number, delayMs: number, error: unknown) => void;
}

const defaultSleep = (ms: number): Promise<void> => new Promise((r) => setTimeout(r, ms));

export async function withRetry<T>(fn: () => Promise<T>, options: RetryOptions): Promise<T> {
  const sleep = options.sleep ?? defaultSleep;
  const random = options.random ?? Math.random;
  const isRetryable =
    options.isRetryable ?? ((error: unknown) => (error as RetryableError)?.retryable !== false);

  let attempt = 0;
  for (;;) {
    try {
      return await fn();
    } catch (error) {
      attempt += 1;
      if (attempt > options.retries || !isRetryable(error)) {
        throw error;
      }
      const hinted = (error as RetryableError)?.retryAfterSeconds;
      const backoff = Math.min(options.maxDelayMs, options.baseDelayMs * 2 ** (attempt - 1));
      const jitter = backoff * 0.25 * random();
      const delayMs = hinted !== undefined ? hinted * 1000 : backoff + jitter;
      options.onRetry?.(attempt, delayMs, error);
      await sleep(delayMs);
    }
  }
}
