import { useCallback, useEffect, useRef, useState } from "react";
import type {
  OutputParams,
  PaintParams,
  WorkerRequest,
  WorkerResponse,
} from "@/engine/types";

export interface ProcessResult {
  imageData: ImageData;
  elapsedMs: number;
}

interface Pending {
  resolve: (r: ProcessResult) => void;
  reject: (e: Error) => void;
}

/**
 * Owns the paint worker.
 * `process` resolves with the painted image;
 * results arriving for superseded request ids are still resolved to
 * their own callers, so the UI can simply ignore stale promises.
 */
export function usePaintEngine() {
  const workerRef = useRef<Worker | null>(null);
  const pendingRef = useRef(new Map<number, Pending>());
  const idRef = useRef(0);
  const [ready, setReady] = useState(false);

  useEffect(() => {
    const worker = new Worker(
      new URL("../engine/paint.worker.ts", import.meta.url),
      { type: "module" },
    );
    workerRef.current = worker;

    worker.onmessage = (ev: MessageEvent<WorkerResponse>) => {
      const msg = ev.data;
      if (msg.type === "ready") {
        setReady(true);
        return;
      }
      const pending = pendingRef.current.get(msg.id);
      if (!pending) return;
      pendingRef.current.delete(msg.id);
      if (msg.type === "error") {
        pending.reject(new Error(msg.message));
      } else {
        const data = new ImageData(
          new Uint8ClampedArray(msg.pixels),
          msg.width,
          msg.height,
        );
        pending.resolve({ imageData: data, elapsedMs: msg.elapsedMs });
      }
    };

    return () => {
      worker.terminate();
      workerRef.current = null;
      pendingRef.current.clear();
      setReady(false);
    };
  }, []);

  const process = useCallback(
    (
      image: ImageData,
      params: PaintParams,
      output?: OutputParams,
    ): Promise<ProcessResult> => {
      const worker = workerRef.current;
      if (!worker) return Promise.reject(new Error("worker not started"));

      const id = ++idRef.current;
      const pixels = image.data.buffer.slice(0) as ArrayBuffer;
      const req: WorkerRequest = {
        type: "process",
        id,
        width: image.width,
        height: image.height,
        pixels,
        params,
        output,
      };
      return new Promise<ProcessResult>((resolve, reject) => {
        pendingRef.current.set(id, { resolve, reject });
        worker.postMessage(req, [pixels]);
      });
    },
    [],
  );

  return { ready, process };
}
