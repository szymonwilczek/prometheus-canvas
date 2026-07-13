# Prometheus

**Oil-painting transformation of any image. Pure mathematics. Zero AI. Zero servers. Zero cost.**

Upload a photo, move few sliders, and watch it become an impasto oil painting - computed
entirely in your browser by C compiled to WebAssembly. No neural network is consulted.
No pixel ever leaves your machine. No GPU farm burns electricity on your behalf.

---

## Manifesto

This project exists because we are angry, and we think you should be too.

Right now, somewhere, a person is dragging your family photo into a diffusion model,
clicking one button, and charging **$10 for the JPEG**. Somewhere else an "app" charges
$15/month - forever - for "AI oil painting portraits", and underneath the landing page
there is nothing but a file upload form, a single HTTP call to a model someone else
trained on paintings scraped from artists who were never asked, and a paywall bolted in
front of it. Etsy listings sell "hand-crafted digital oil portraits" that are one prompt
and eleven seconds of GPU time. Subscription tiers. Watermarks on the "free" version.
"Credits." For *this*.

Here is what they are hoping you never find out: **the painterly look was solved by
mathematics half a century ago, published openly, and it has been sitting in the public
literature free of charge ever since.** Kuwahara, 1976. Sobel, 1968. Blinn and Phong,
1977. Lanczos, 1979. These people gave their work to everyone, forever. The sellers
wrapped a rental GPU around a scraped model and priced it like craftsmanship.

Nobody painted your $10 "painting". Nobody even *computed* it honestly - they rented the
computation, marked it up, and kept your photo.

**Mathematics was free. Mathematics is free. Mathematics will remain free.**

Prometheus is proof by construction. Every visual effect in this application is a
classic, citable computer-vision or computer-graphics algorithm running in *your* browser
on *your* CPU - no model, no scraping, no rental, no bill:

| Effect | Algorithm | Era |
|---|---|---|
| Painterly smoothing | [Kuwahara filter](https://en.wikipedia.org/wiki/Kuwahara_filter) | 1976 |
| Brush strokes follow contours | Structure tensor + line integral convolution | 1987/1993 |
| Stroke-boundary detection | [Sobel operator](https://en.wikipedia.org/wiki/Sobel_operator) | 1968 |
| 3D paint relief | [Blinn-Phong illumination over a heightmap](https://en.wikipedia.org/wiki/Blinn%E2%80%93Phong_reflection_model) | 1975/1977 |
| Bristle grooves & canvas weave | Procedural phase functions + integer hash noise | classic CG |
| Limited pigment palette | [k-means color quantization](https://en.wikipedia.org/wiki/K-means_clustering) | 1957/1967 |
| 4K upscaling | [Lanczos3 resampling](https://en.wikipedia.org/wiki/Lanczos_resampling) | 1979 |
| Final crispness | [Unsharp masking](https://en.wikipedia.org/wiki/Unsharp_masking) | ~1930s (darkroom technique) |

There is no model file. There is no inference. There is no telemetry. The entire "engine"
is a few kilobytes of freestanding C99 compiled to WebAssembly, and every line of it is in
this repository for you to read, audit, learn from, and steal.

### What I assert

1. **Local-first is a right, not a tier.** Your images are processed in a Web Worker on
   your CPU. The app works offline. There is nothing to subscribe to, because there is
   no recurring cost to pass on - that is what "no servers" actually means.
2. **Mathematics is not obsolete.** A 1976 filter with an O(1)-per-pixel optimization
   produces painterly results at interactive speed on a laptop. Not everything needs a
   billion parameters, and nothing here needed even one.
3. **Engineering beats wrapping.** The difficulty of a product should live in its code,
   not in its pricing page. If your entire product is somebody else's API call, the
   price of your product is somebody else's API price - everything above that is the
   fee you charge for hoping your customers don't know better.
4. **Free means free.** GPL-2.0 licensed (see [LICENSE](LICENSE)), no accounts, no
   watermarks, no "3 free credits" - and copyleft, so it stays free downstream. If
   someone tries to sell you this exact software, the license is on your side: demand
   the source, take it, and run.
5. **Hosted is a convenience, not a leash.** A public instance exists so nobody is
   *forced* to install anything - but you can always clone this repository and run the
   entire application locally, offline, forever. The hosted copy holds nothing hostage:
   no accounts, no quotas, no telemetry. See [Deploying](#deploying-vercel-or-anywhere-static)
   to stand up your own copy in one command.

If you were about to pay $5/month - or $10 per image - to make your photos look like
paintings: don't. Every algorithm the sellers hide behind a checkout button is in this
repository, named, documented, and free. Clone it. Read it. Keep your money and keep
your photos.

---

## How it works - mathematics

The pipeline runs in this order, entirely inside a Web Worker hosting the WASM module:

```
[INPUT] RGBA input

[1] Kuwahara filter          - painterly foundation (paint splotches)
[2] k-means quantization     - limited physical pigment simulation
[3] Flow-guided strokes      - LIC along the structure tensor: splotches
                               stretch into strokes that bend with contours
[4] Saturation & contrast    - oil-paint vibrance
[5] Heightmap synthesis      - Sobel ridges ^1.4 + bristle grooves
                               + procedural plain-weave canvas
[6] Blinn-Phong shading      - impasto relief with a movable light source
[7] Lanczos3 resampling      - mathematical upscaling to 2x/4x
[8] Unsharp mask             - final output crispness

[OUTPUT] RGBA output
```

### 1. Kuwahara filter

For each pixel, four overlapping square sectors (NW, NE, SW, SE) of radius *r* are
examined. Each sector's mean color and luminance variance come from *anchored
sliding-window box sums* (the streaming cousin of summed-area tables): one separable
pass computes every window sum in O(1) amortized per pixel, and the four sectors of a
pixel are that window sampled at four offsets:

```math
running \sum:  add row[x], subtract row[x-r-1]   (then same vertically)
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

### 3. Flow-guided brush strokes (structure tensor + LIC)

A painter drags the brush *along* contours. The smoothed **structure tensor**
(Sobel gradient outer product, Gaussian-blurred) encodes local orientation; its minor
eigenvector - computed in closed form from the 2×2 symmetric eigensystem, no `atan2` -
points along image flow, and the eigenvalue spread gives an anisotropy measure that is
zero in flat regions. Each pixel's color is then advected up to *L* steps both ways along
the field (**line integral convolution**, Cabral & Leedom 1993), re-reading the field at
every step so strokes bend around curves. Running this *after* quantization smears hard
pigment boundaries into directional, brushy transitions.

### 4. Saturation & contrast

Plain per-pixel linear algebra around the Rec. 601 luma axis:
$\`out = luma + (in − luma) \cdot s\`$, then $\`out = (out − 0.5) \cdot c + 0.5\`$.

### 5–6. Impasto: heightmap synthesis + Blinn-Phong illumination

Oil paint is three-dimensional; light rakes across ridges of paint. The height field is
assembled from three mathematically distinct layers:

- **Ridges** - Sobel gradient magnitude of the painted image, raised to the power 1.4
  (bold stroke edges keep their paint bead, faint pigment-band outlines fade), then
  box-blurred so ridges have flanks for light to catch.
- **Bristle grooves** - a sine phase running *across* the local stroke direction,
  phase-jittered by integer hash noise so individual hairs stay irregular, gated by
  anisotropy^2 so grooves exist only along genuine strokes.
- **Canvas weave** - procedural plain-weave fabric: warp/weft threads alternate
  over/under on a checkerboard, each thread crowned by |sin|, thickness jittered per
  cell - attenuated wherever paint is thick. Heavy impasto hides the canvas; thin
  washes reveal it, exactly like a real painting.
- Surface normals are derived from the heightmap gradient, scaled by the **Impasto
  Depth** slider: $\`N = normalize(\frac{-\delta h}{\delta x} \cdot d, \frac{-\delta h}{\delta y} \cdot d, 1)\`$.
- **Blinn-Phong** model lights every pixel:
  $\`L = ambient + diffuse \cdot max(N \ cdot \hat{L}, 0) + specular \ cdot max(N \cdot \hat{H}, 0)^{shininess}\`$
  where the light direction $\`\hat{L}\`$ is computed from the **Elevation** and **Azimuth**
  sliders, and $\`\hat{H}\`$ is the half-vector. Drag the azimuth and watch the paint ridges cast
  shadows from the other side - that is a real illumination model, not a filter preset.

### 7. Lanczos3 resampling

Upscaling uses the Lanczos kernel with a = 3:

```math
L(x) = sinc(x) \cdot sinc(x/3)   for |x| < 3,   0 otherwise
sinc(x) = \frac{sin(\pi x)}{\pi x}
```

Implemented as two separable passes (horizontal, then vertical) with per-destination
precomputed kernel weights. Lanczos3 is the classical gold standard for image resampling -
the same mathematics your photo editor uses when you pick "best quality".

### 8. Unsharp mask

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

## Deploying (Vercel, or anywhere static)

Repository is deploy-ready: `vercel.json` configures the build command, output
directory and cache headers, and because the WASM artifact is committed, **the deploy
machine needs no C toolchain** - Vercel just runs `npm run build` and ships static files.

```sh
npx vercel        # from the repo root; that's the whole procedure
```

Any other static host (Netlify, GitHub Pages, an nginx box, a USB stick) works the same
way: `npm run build`, serve `dist/`. There is no server-side component whatsoever - the
"backend" is your visitors' own CPUs.

To be absolutely clear about the philosophy: hosted instance exists so that people
don't *have* to set anything up locally. You still **can** - clone it, build it, run it
offline forever. The hosted copy is a convenience, not a leash: it has no accounts, no
quotas, no telemetry, and nothing to upsell. If it ever disappears, nothing is lost;
every visitor already had the entire engine in their browser cache.

## License

GPL-2.0. Take it, ship it, but keep it open. That's the point.
