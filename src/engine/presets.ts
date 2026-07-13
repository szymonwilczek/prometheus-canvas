import { DEFAULT_PAINT_PARAMS, type PaintParams } from "./types";

export interface Preset {
  id: string;
  name: string;
  params: PaintParams;
}

/**
 * One-click starting points.
 * Values are hand-tuned against rendered test scenes;
 * every preset is just a slider configuration - there is no hidden logic behind them.
 */
export const PRESETS: Preset[] = [
  {
    id: "classic-oil",
    name: "Classic Oil",
    params: { ...DEFAULT_PAINT_PARAMS },
  },
  {
    id: "thick-impasto",
    name: "Thick Impasto",
    params: {
      ...DEFAULT_PAINT_PARAMS,
      kuwaharaRadius: 9,
      edgeQ: 10,
      strokeLength: 12,
      pigments: 16,
      saturation: 1.25,
      contrast: 1.1,
      impastoDepth: 85,
      lightElevation: 35,
      specular: 0.45,
      shininess: 20,
      bristle: 0.85,
      canvasWeave: 0.25,
      cavity: 0.8,
      pigmentNoise: 0.6,
      noiseScale: 10,
    },
  },
  {
    id: "palette-knife",
    name: "Palette Knife",
    params: {
      ...DEFAULT_PAINT_PARAMS,
      mode: "knife",
      kuwaharaRadius: 4,
      edgeQ: 8,
      strokeLength: 0,
      pigments: 26,
      saturation: 1.5,
      contrast: 1.2,
      impastoDepth: 32,
      specular: 0.35,
      shininess: 36,
      bristle: 0,
      canvasWeave: 0,
      cavity: 0.4,
      pigmentNoise: 0.3,
      noiseScale: 10,
      knifeSize: 40,
      knifeDetail: 0.65,
    },
  },
  {
    id: "fine-brush",
    name: "Fine Brush",
    params: {
      ...DEFAULT_PAINT_PARAMS,
      kuwaharaRadius: 3,
      edgeQ: 6,
      strokeLength: 6,
      pigments: 36,
      saturation: 1.15,
      contrast: 1.05,
      impastoDepth: 30,
      specular: 0.25,
      shininess: 32,
      bristle: 0.5,
      canvasWeave: 0.35,
      weaveScale: 3,
      cavity: 0.35,
      pigmentNoise: 0.3,
      noiseScale: 6,
    },
  },
  {
    id: "soft-wash",
    name: "Soft Wash",
    params: {
      ...DEFAULT_PAINT_PARAMS,
      kuwaharaRadius: 8,
      edgeQ: 2,
      strokeLength: 16,
      pigments: 28,
      saturation: 1.05,
      contrast: 1.0,
      impastoDepth: 18,
      specular: 0.1,
      shininess: 16,
      bristle: 0.25,
      canvasWeave: 0.55,
      weaveScale: 5,
      cavity: 0.25,
      pigmentNoise: 0.4,
      noiseScale: 12,
    },
  },
];
