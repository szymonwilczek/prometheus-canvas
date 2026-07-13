import { useCallback, useEffect, useRef, useState } from "react";
import { Button } from "@/components/ui/button";
import { Badge } from "@/components/ui/badge";
import { Separator } from "@/components/ui/separator";
import { ControlsPanel } from "@/components/controls-panel";
import { Preview } from "@/components/preview";
import { usePaintEngine } from "@/hooks/use-paint-engine";
import {
  DEFAULT_OUTPUT_PARAMS,
  DEFAULT_PAINT_PARAMS,
  type OutputParams,
  type PaintParams,
} from "@/engine/types";

/** Longest preview proxy edge; full resolution is used only on export. */
const PREVIEW_MAX = 1024;
/** Hard cap on the export output's longest edge (bounds WASM memory). */
const EXPORT_MAX = 4096;

function bitmapToImageData(bmp: ImageBitmap, maxEdge?: number): ImageData {
  let { width: w, height: h } = bmp;
  if (maxEdge && Math.max(w, h) > maxEdge) {
    const s = maxEdge / Math.max(w, h);
    w = Math.round(w * s);
    h = Math.round(h * s);
  }
  const canvas = document.createElement("canvas");
  canvas.width = w;
  canvas.height = h;
  const ctx = canvas.getContext("2d")!;
  ctx.drawImage(bmp, 0, 0, w, h);
  return ctx.getImageData(0, 0, w, h);
}

function downloadPng(image: ImageData, filename: string) {
  const canvas = document.createElement("canvas");
  canvas.width = image.width;
  canvas.height = image.height;
  canvas.getContext("2d")!.putImageData(image, 0, 0);
  canvas.toBlob((blob) => {
    if (!blob) return;
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = filename;
    a.click();
    URL.revokeObjectURL(url);
  }, "image/png");
}

function App() {
  const { ready, process } = usePaintEngine();
  const [fullImage, setFullImage] = useState<ImageData | null>(null);
  const [proxy, setProxy] = useState<ImageData | null>(null);
  const [painted, setPainted] = useState<ImageData | null>(null);
  const [params, setParams] = useState<PaintParams>(DEFAULT_PAINT_PARAMS);
  const [output, setOutput] = useState<OutputParams>(DEFAULT_OUTPUT_PARAMS);
  const [elapsedMs, setElapsedMs] = useState<number | null>(null);
  const [busy, setBusy] = useState(false);
  const [exporting, setExporting] = useState(false);
  const fileRef = useRef<HTMLInputElement>(null);
  const jobRef = useRef(0);

  const loadFile = useCallback(async (file: File) => {
    const bmp = await createImageBitmap(file, {
      imageOrientation: "from-image",
    });
    setFullImage(bitmapToImageData(bmp));
    setProxy(bitmapToImageData(bmp, PREVIEW_MAX));
    setPainted(null);
    bmp.close();
  }, []);

  /* debounced preview: stages 1-5 at proxy resolution, no upscale */
  useEffect(() => {
    if (!ready || !proxy) return;
    const job = ++jobRef.current;
    setBusy(true);
    const t = setTimeout(() => {
      process(proxy, params)
        .then((r) => {
          if (jobRef.current !== job) return;
          setPainted(r.imageData);
          setElapsedMs(r.elapsedMs);
          setBusy(false);
        })
        .catch(() => {
          if (jobRef.current === job) setBusy(false);
        });
    }, 120);
    return () => clearTimeout(t);
  }, [ready, proxy, params, process]);

  const onExport = async () => {
    if (!ready || !fullImage || !proxy) return;
    setExporting(true);
    try {
      const ratio = fullImage.width / proxy.width;
      const scaled: PaintParams = {
        ...params,
        kuwaharaRadius: Math.round(params.kuwaharaRadius * ratio),
        strokeLength: Math.round(params.strokeLength * ratio),
        weaveScale: Math.max(2, Math.round(params.weaveScale * ratio)),
      };
      const longest = Math.max(fullImage.width, fullImage.height);
      const factor = Math.min(output.upscaleFactor, EXPORT_MAX / longest);
      const r = await process(fullImage, scaled, {
        ...output,
        upscaleFactor: Math.max(1, factor),
      });
      downloadPng(r.imageData, "prometheus-canvas.png");
    } finally {
      setExporting(false);
    }
  };

  return (
    <div className="flex h-screen flex-col bg-background text-foreground">
      <header className="flex flex-wrap items-center gap-x-3 gap-y-2 border-b px-4 py-2">
        <h1 className="font-mono text-sm font-semibold">prometheus-canvas</h1>
        <Badge
          variant="outline"
          className="hidden font-mono text-[10px] sm:inline-flex"
        >
          pure math · zero AI · zero servers
        </Badge>
        <div className="ml-auto flex items-center gap-2">
          {fullImage && (
            <span className="font-mono text-xs text-muted-foreground tabular-nums">
              {fullImage.width}×{fullImage.height}
            </span>
          )}
          <input
            ref={fileRef}
            type="file"
            accept="image/*"
            className="hidden"
            onChange={(e) => {
              const f = e.target.files?.[0];
              if (f) void loadFile(f);
              e.target.value = "";
            }}
          />
          <Button
            variant="outline"
            size="sm"
            onClick={() => fileRef.current?.click()}
          >
            Upload image
          </Button>
          <Button
            size="sm"
            disabled={!fullImage || !ready || exporting}
            onClick={() => void onExport()}
          >
            {exporting ? "Rendering…" : `Export PNG ${output.upscaleFactor}x`}
          </Button>
        </div>
      </header>

      <div className="flex min-h-0 flex-1 flex-col lg:flex-row">
        <main
          className="h-[52vh] shrink-0 p-3 lg:h-auto lg:min-w-0 lg:flex-1"
          onDragOver={(e) => e.preventDefault()}
          onDrop={(e) => {
            e.preventDefault();
            const f = e.dataTransfer.files?.[0];
            if (f?.type.startsWith("image/")) void loadFile(f);
          }}
        >
          <Preview
            original={proxy}
            painted={painted}
            elapsedMs={elapsedMs}
            busy={busy}
          />
        </main>

        <aside className="min-h-0 flex-1 overflow-y-auto border-t p-3 lg:w-72 lg:flex-none lg:border-t-0 lg:border-l">
          <div className="mb-2 flex items-center justify-between">
            <span className="text-xs font-medium text-muted-foreground">
              PIPELINE PARAMETERS
            </span>
            <Badge
              variant={ready ? "secondary" : "destructive"}
              className="text-[10px]"
            >
              {ready ? "wasm ready" : "loading wasm"}
            </Badge>
          </div>
          <Separator className="mb-3" />
          <ControlsPanel
            params={params}
            output={output}
            onParams={setParams}
            onOutput={setOutput}
          />
        </aside>
      </div>
    </div>
  );
}

export default App;
