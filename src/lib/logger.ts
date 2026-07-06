import { pino } from 'pino';

export type Logger = import('pino').Logger;

let root: Logger | undefined;

export interface LoggerOptions {
  level: string;
  pretty: boolean;
}

/** Create (once) and return the root logger. */
export function initLogger(options: LoggerOptions): Logger {
  root = pino({
    level: options.level,
    ...(options.pretty
      ? { transport: { target: 'pino-pretty', options: { translateTime: 'SYS:HH:MM:ss' } } }
      : {}),
  });
  return root;
}

/** Get the root logger, or a no-frills default if init hasn't run yet. */
export function getLogger(): Logger {
  if (!root) {
    root = pino({ level: process.env.LOG_LEVEL ?? 'info' });
  }
  return root;
}
