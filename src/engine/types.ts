/**
 * Rendering mode: multi-scale traced impasto strokes (default), discrete
 * knife smears, or the legacy per-pixel LIC brush filter.
 */
export type PaintMode = "brush" | "knife" | "sbr";

/** Parameters for the constant-resolution paint pipeline (stages 1-5). */
export interface PaintParams {
  /**
   * Renderer: 'sbr' (layered flow-traced strokes with a physical heightmap,
   * the default), 'knife' (stroke-based smears), or 'brush' (legacy
   * Kuwahara + flow LIC filter, kept for compatibility - no paint physics).
   */
  mode: PaintMode;
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
  /** Cavity shading strength (0-1): ambient occlusion in paint valleys. */
  cavity: number;
  /** Pigment mixing imperfection (0-1): value-noise luminance modulation. */
  pigmentNoise: number;
  /** Pigment noise patch size in px. */
  noiseScale: number;
  /** Knife/SBR modes: largest smear/slab size in px (coarsest layer). */
  knifeSize: number;
  /** Knife mode: 0 = broad slabs only, 1 = dense fine detail. */
  knifeDetail: number;
  /**
   * Knife mode: blade tilting / edge ridges (0-1). Scales the twin
   * squeezed paint beads along the smear margins vs a flat slab.
   */
  knifeRidge: number;
  /**
   * Knife + SBR modes: paint dryness / skipping threshold (0-1). 0 = loaded
   * wet brush covers everything, 1 = pigment catches only on relief peaks,
   * leaving dry-brush voids in the valleys.
   */
  knifeDryness: number;
  /**
   * Knife + SBR modes: wet-on-wet pigment dragging (0-1). How much of the
   * underlying wet paint the stroke shears, carries, and (for SBR) picks up
   * into its contaminating brush reservoir.
   */
  knifeDrag: number;
  /**
   * Knife + SBR modes: pigment mix vibrancy (0-1). Strength of subtractive
   * Kubelka-Munk paint mixing vs classic linear RGB blending.
   */
  paintVibrancy: number;
  /**
   * Knife + SBR modes: brush anisotropy (0-1). Blends the isotropic
   * Blinn-Phong highlight toward a Kajiya-Kay strand glint stretched
   * across the bristle grooves.
   */
  brushAnisotropy: number;
  /**
   * Knife + SBR modes: edge fringing (0-1). Intensity of the darker,
   * more saturated pigment halo just inside every stroke boundary.
   */
  edgeFringe: number;
  /** SBR: paint the undercoat layer (broad flat slabs). */
  sbrUndercoat: boolean;
  /** SBR: undercoat stroke density multiplier (0.25-2). */
  sbrUndercoatDensity: number;
  /** SBR: paint the form layer (medium flow-traced strokes). */
  sbrForm: boolean;
  /** SBR: form stroke density multiplier (0.25-2). */
  sbrFormDensity: number;
  /** SBR: paint the detail layer (importance-gated micro-strokes). */
  sbrDetail: boolean;
  /** SBR: detail stroke density multiplier (0.25-2). */
  sbrDetailDensity: number;
  /** SBR: 1 = strokes bend fully with the flow field, 0 = straight flicks. */
  sbrAlignment: number;
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
  mode: "sbr",
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
  cavity: 0.5,
  pigmentNoise: 0.5,
  noiseScale: 9,
  knifeSize: 40,
  knifeDetail: 0.65,
  knifeRidge: 0.6,
  knifeDryness: 0.25,
  knifeDrag: 0.45,
  paintVibrancy: 0.65,
  brushAnisotropy: 0.6,
  edgeFringe: 0.35,
  sbrUndercoat: true,
  sbrUndercoatDensity: 1,
  sbrForm: true,
  sbrFormDensity: 1,
  sbrDetail: true,
  sbrDetailDensity: 1,
  sbrAlignment: 0.85,
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
