/**
 * Web Worker hosting the freestanding WASM engine.
 * Keeps the main thread at 60 fps:
 * all pixel work happens here, images cross the boundary
 * as transferable ArrayBuffers.
 */
import type { WorkerRequest, WorkerResponse } from "./types";

interface EngineExports {
  memory: WebAssembly.Memory;
  pc_reset(): void;
  pc_alloc(n: number): number;
  pc_process(
    src: number,
    w: number,
    h: number,
    dst: number,
    kuwaharaRadius: number,
    edgeQ: number,
    strokeLength: number,
    pigments: number,
    saturation: number,
    contrast: number,
    impastoDepth: number,
    lightElev: number,
    lightAzim: number,
    specular: number,
    shininess: number,
    bristle: number,
    weave: number,
    weaveScale: number,
    cavity: number,
    pigmentNoise: number,
    noiseScale: number,
    renderMode: number,
    knifeSize: number,
    knifeDetail: number,
    sbrUndercoat: number,
    sbrForm: number,
    sbrDetail: number,
    sbrAlignment: number,
    knifeRidge: number,
    knifeDryness: number,
    knifeDrag: number,
    vibrancy: number,
    anisotropy: number,
    fringe: number,
    sssScatter: number,
    sssAbsorb: number,
    varnish: number,
    varnishIor: number,
    glossDep: number,
    crackTension: number,
    crackDepth: number,
    crackDirt: number,
    warpTension: number,
    warpPoisson: number,
    wrinkleFreq: number,
    illuminant: number,
    spectral: number,
  ): void;
  pc_upscale(
    src: number,
    sw: number,
    sh: number,
    dst: number,
    dw: number,
    dh: number,
    unsharpAmount: number,
    unsharpSigma: number,
  ): void;
}

let engine: EngineExports | null = null;

const post = (msg: WorkerResponse, transfer: Transferable[] = []) =>
  (self as unknown as DedicatedWorkerGlobalScope).postMessage(msg, transfer);

async function init(): Promise<void> {
  const res = await fetch("/wasm/prometheus.wasm");
  const { instance } = await WebAssembly.instantiate(await res.arrayBuffer());
  engine = instance.exports as unknown as EngineExports;
  post({ type: "ready" });
}

const DEG = Math.PI / 180;

/* keep ids in sync with the PC_ILL_* constants in engine/pc.h */
const ILLUMINANT_ID = { d65: 0, a: 1, f11: 2, candle: 3 } as const;

function handle(req: WorkerRequest): void {
  if (!engine) throw new Error("engine not initialized");
  const t0 = performance.now();
  const { width: w, height: h, params: p } = req;

  engine.pc_reset();
  const srcPtr = engine.pc_alloc(w * h * 4);

  const out = req.output;
  const scale = out ? out.upscaleFactor : 1;
  const dw = Math.round(w * scale);
  const dh = Math.round(h * scale);

  const dstPtr = engine.pc_alloc(w * h * 4);
  const finalPtr = scale !== 1 ? engine.pc_alloc(dw * dh * 4) : dstPtr;

  // memory may have grown during pc_alloc: create views afterwards
  new Uint8Array(engine.memory.buffer, srcPtr, w * h * 4).set(
    new Uint8Array(req.pixels),
  );

  engine.pc_process(
    srcPtr,
    w,
    h,
    dstPtr,
    p.kuwaharaRadius,
    p.edgeQ,
    p.strokeLength,
    p.pigments,
    p.saturation,
    p.contrast,
    p.impastoDepth,
    p.lightElevation * DEG,
    p.lightAzimuth * DEG,
    p.specular,
    p.shininess,
    p.bristle,
    p.canvasWeave,
    p.weaveScale,
    p.cavity,
    p.pigmentNoise,
    p.noiseScale,
    p.mode === "knife" ? 1 : p.mode === "sbr" ? 2 : 0,
    p.knifeSize,
    p.knifeDetail,
    p.sbrUndercoat ? p.sbrUndercoatDensity : 0,
    p.sbrForm ? p.sbrFormDensity : 0,
    p.sbrDetail ? p.sbrDetailDensity : 0,
    p.sbrAlignment,
    p.knifeRidge,
    p.knifeDryness,
    p.knifeDrag,
    p.paintVibrancy,
    p.brushAnisotropy,
    p.edgeFringe,
    p.sssScatter,
    p.sssAbsorb,
    p.varnishThickness,
    p.varnishIor,
    p.glossDependency,
    p.crackTension,
    p.crackDepth,
    p.crackAge,
    p.warpTension,
    p.warpPoisson,
    p.wrinkleFrequency,
    ILLUMINANT_ID[p.illuminant],
    p.spectralIntensity,
  );

  if (out) {
    if (scale !== 1) {
      engine.pc_upscale(
        dstPtr,
        w,
        h,
        finalPtr,
        dw,
        dh,
        out.unsharpAmount,
        out.unsharpSigma,
      );
    } else if (out.unsharpAmount > 0) {
      // in-place unsharp via the same export with identical dimensions
      // would copy dst->dst; run the resample path into fresh buffer
      const tmpPtr = engine.pc_alloc(dw * dh * 4);
      engine.pc_upscale(
        dstPtr,
        w,
        h,
        tmpPtr,
        dw,
        dh,
        out.unsharpAmount,
        out.unsharpSigma,
      );
      new Uint8Array(engine.memory.buffer, dstPtr, dw * dh * 4).set(
        new Uint8Array(engine.memory.buffer, tmpPtr, dw * dh * 4),
      );
    }
  }

  // views must be re-taken:
  // internal stage allocations may have grown memory and detached earlier views
  const result = new Uint8Array(
    engine.memory.buffer,
    finalPtr,
    dw * dh * 4,
  ).slice();

  post(
    {
      type: "result",
      id: req.id,
      width: dw,
      height: dh,
      pixels: result.buffer,
      elapsedMs: performance.now() - t0,
    },
    [result.buffer],
  );
}

self.onmessage = (ev: MessageEvent<WorkerRequest>) => {
  try {
    handle(ev.data);
  } catch (err) {
    post({
      type: "error",
      id: ev.data.id,
      message: err instanceof Error ? err.message : String(err),
    });
  }
};

void init();
