/** Parameters for the constant-resolution paint pipeline (stages 1-5). */
export interface PaintParams {
  /** Kuwahara brush radius in px, 0 disables. Range 0-30. */
  kuwaharaRadius: number;
  /** Edge preservation exponent q. Low = soft strokes, high = hard. */
  edgeQ: number;
  /** Flow-guided stroke length in px (LIC along the structure tensor), 0 disables. */
  strokeLength: number;
  /** Number of pigment clusters, 0 disables quantization. Range 0-64. */
  pigments: number;
  /** Chroma multiplier around the luma axis. 1 = neutral. */
  saturation: number;
  /** Linear contrast about mid-gray. 1 = neutral. */
  contrast: number;
  /** Heightmap gradient multiplier for the Blinn-Phong normals. 0 disables. */
  impastoDepth: number;
  /** Light elevation above the canvas plane, degrees (0-90). */
  lightElevation: number;
  /** Light azimuth, degrees (0 = east, counter-clockwise). */
  lightAzimuth: number;
  /** Specular strength (0-1). */
  specular: number;
  /** Blinn-Phong shininess exponent (integer). */
  shininess: number;
  /** Bristle groove strength (0-1), gated by local anisotropy. */
  bristle: number;
  /** Canvas weave strength (0-1), revealed where paint is thin. */
  canvasWeave: number;
  /** Weave thread pitch in px. */
  weaveScale: number;
}

/** Parameters for the output stage (stages 6-7). */
export interface OutputParams {
  /** Lanczos3 upscale factor: 1, 2 or 4. */
  upscaleFactor: number;
  /** Unsharp mask amount, 0 disables. */
  unsharpAmount: number;
  /** Unsharp mask Gaussian sigma. */
  unsharpSigma: number;
}

export const DEFAULT_PAINT_PARAMS: PaintParams = {
  kuwaharaRadius: 7,
  edgeQ: 8,
  strokeLength: 10,
  pigments: 20,
  saturation: 1.2,
  contrast: 1.08,
  impastoDepth: 50,
  lightElevation: 45,
  lightAzimuth: 130,
  specular: 0.3,
  shininess: 24,
  bristle: 0.6,
  canvasWeave: 0.3,
  weaveScale: 4,
};

export const DEFAULT_OUTPUT_PARAMS: OutputParams = {
  upscaleFactor: 2,
  unsharpAmount: 0.4,
  unsharpSigma: 1.2,
};

/** Messages into the worker. */
export type WorkerRequest = {
  type: "process";
  id: number;
  width: number;
  height: number;
  pixels: ArrayBuffer;
  params: PaintParams;
  output?: OutputParams;
};

/** Messages out of the worker. */
export type WorkerResponse =
  | { type: "ready" }
  | {
      type: "result";
      id: number;
      width: number;
      height: number;
      pixels: ArrayBuffer;
      elapsedMs: number;
    }
  | { type: "error"; id: number; message: string };
