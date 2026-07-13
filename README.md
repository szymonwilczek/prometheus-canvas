# Prometheus

**Oil-painting transformation of any image. Pure mathematics. Zero AI. Zero servers. Zero cost.**

Upload a photo, move few sliders, and watch it become an impasto oil painting - computed
entirely in your browser by C compiled to WebAssembly. No neural network is consulted.
No pixel ever leaves your machine. No GPU farm burns electricity on your behalf.

---

## Manifesto

This project exists as a deliberate counter-movement.

The current software landscape is flooded with "AI photo apps" that are nothing more than
thin wrappers: a file upload form, one HTTP call to a diffusion-model API someone else built,
and a subscription paywall in front of it. They charge predatory fees for what is,
underneath, a single automated request. They rent GPU clusters to do work that - for an entire
class of problems - never needed a GPU cluster at all.

**Prometheus is proof by construction that this class of problem was solved decades
ago, by mathematics, and that the solution runs on the device you already own.**

Every visual effect in this application is a classic, citable computer-vision or
computer-graphics algorithm:

| Effect | Algorithm | Era |
|---|---|---|
| Painterly smoothing | [Kuwahara filter](https://en.wikipedia.org/wiki/Kuwahara_filter) | 1976 |
| Stroke-boundary detection | [Sobel operator](https://en.wikipedia.org/wiki/Sobel_operator) | 1968 |
| 3D paint relief | [Blinn-Phong illumination over a heightmap](https://en.wikipedia.org/wiki/Blinn%E2%80%93Phong_reflection_model) | 1975/1977 |
| Limited pigment palette | [k-means color quantization](https://en.wikipedia.org/wiki/K-means_clustering) | 1957/1967 |
| 4K upscaling | [Lanczos3 resampling](https://en.wikipedia.org/wiki/Lanczos_resampling) | 1979 |
| Final crispness | [Unsharp masking](https://en.wikipedia.org/wiki/Unsharp_masking) | ~1930s (darkroom technique) |

There is no model file. There is no inference. There is no telemetry. The entire "engine"
is a few kilobytes of freestanding C99 compiled to WebAssembly, and every line of it is in
this repository for you to read, audit, learn from, and steal.

### What I assert

1. **Local-first is a right, not a tier.** Your images are processed in a Web Worker on
   your CPU. The app works offline. There is nothing to subscribe to.
2. **Mathematics is not obsolete.** A 1976 filter with a summed-area-table optimization
   produces painterly results at interactive speed on a laptop. Not everything needs a
   billion parameters.
3. **Engineering beats wrapping.** The difficulty of a product should live in its code,
   not in its pricing page.
4. **Free means free.** GPL-2.0 licensed (see [LICENSE](LICENSE)), no accounts, no
   watermarks, no "3 free credits" - and copyleft, so it stays free downstream.

If you were about to pay \$15/month or \$10 per phototo make your photos look like paintings: don't.
Clone this instead.

---

## How it works - mathematics

The pipeline runs in this order, entirely inside a Web Worker hosting the WASM module:

```
[INPUT] RGBA input

[1] Kuwahara filter          - painterly foundation (paint splotches)
[2] k-means quantization     - limited physical pigment simulation
[3] Saturation & contrast    - oil-paint vibrance
[4] Sobel heightmap          - where does paint pile up?
[5] Blinn-Phong shading      - impasto relief with a movable light source
[6] Lanczos3 resampling      - mathematical upscaling to 2x/4x
[7] Unsharp mask             - final output crispness

[OUTPUT] RGBA output
```

### 1. Kuwahara filter

For each pixel, four overlapping square sectors (NW, NE, SW, SE) of radius *r* are
examined. Each sector's mean color and luminance variance are computed in **O(1)** per
sector using integral images (summed-area tables) over both channel sums and squared sums:

```math
\sum -table:  S\left(x,y\right) = I\left(x,y\right) + S\left(x-1,y\right) + S\left(x,y-1\right) - S\left(x-1,y-1\right)
{\sigma}^2 = E\left[X^2\right] − {\left(E\left[X\right]\right)}^2
```

Classic filter picks the sector with minimum variance. I implement *generalized*
weighted form: every sector contributes with weight $\`w = 1 / (1 + {\sigma}^2)^q\`$, where the
exponent `q` is **Edge Preservation** slider.

High `q` collapses to the classic minimum-variance choice (hard, sharp stroke boundaries);
low `q` blends sectors softly.

This turns homogeneous regions into flat *paint splotches* while refusing to average
across structural edges - the mathematical core of the painted look.

Total cost is O(W x H) regardless of brush radius, which is why the brush-size slider stays
interactive up to 30 px.

### 2. Color quantization

Real painters mix a finite palette. I simulate this by clustering all image colors into
*k* centroids in RGB space (k = the **Pigment Count** slider), using deterministic
luminance-stratified initialization followed by Lloyd iterations on a subsampled pixel
set, then snapping every pixel to its nearest centroid. Deterministic seeding means the
same image + same k always yields the same palette - reproducibility over vibes.

### 3. Saturation & contrast

Plain per-pixel linear algebra around the Rec. 601 luma axis:
$\`out = luma + (in − luma) \cdot s\`$, then $\`out = (out − 0.5) \cdot c + 0.5\`$.

### 4-5. Impasto

Oil paint is three-dimensional; light rakes across ridges of paint. I reconstruct this:

- Grayscale **heightmap** is built from the Sobel gradient magnitude of the painted
  image - paint *piles up* along stroke boundaries. The map is box-blurred so ridges have
  flanks for light to catch.
- Surface normals are derived from the heightmap gradient, scaled by the **Impasto
  Depth** slider: $\`N = normalize(\frac{-\delta h}{\delta x} \cdot d, \frac{-\delta h}{\delta y} \cdot d, 1)\`$.
- **Blinn-Phong** model lights every pixel:
  $\`L = ambient + diffuse \cdot max(N \ cdot \hat{L}, 0) + specular \ cdot max(N \cdot \hat{H}, 0)^{shininess}\`$
  where the light direction $\`\hat{L}\`$ is computed from the **Elevation** and **Azimuth**
  sliders, and $\`\hat{H}\`$ is the half-vector. Drag the azimuth and watch the paint ridges cast
  shadows from the other side - that is a real illumination model, not a filter preset.

### 6. Lanczos3 resampling

Upscaling uses the Lanczos kernel with a = 3:

```math
L(x) = sinc(x) · sinc(x/3)   for |x| < 3,   0 otherwise
sinc(x) = \frac{sin(\pi x)}{\pi x}
```

Implemented as two separable passes (horizontal, then vertical) with per-destination
precomputed kernel weights. Lanczos3 is the classical gold standard for image resampling -
the same mathematics your photo editor uses when you pick "best quality".

### 7. Unsharp mask

$\`out = in + amount \cdot (in − gaussian(in))\`$ - the digital descendant of a darkroom
technique that predates the transistor.

## Building

```sh
npm install
npm run build:wasm   # requires clang + wasm-ld (package 'lld' on Fedora)
npm run dev
```

Compiled `public/wasm/prometheus.wasm` artifact is committed, so `npm run dev` works
without a C toolchain; `build:wasm` is only needed when you modify the C sources in
`engine/`.

## License

GPL-2.0. Take it, ship it, but keep it open. That's the point.
