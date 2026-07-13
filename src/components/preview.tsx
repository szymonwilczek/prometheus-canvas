import { useEffect, useRef, useState } from "react";
import { Tabs, TabsList, TabsTrigger } from "@/components/ui/tabs";
import { Badge } from "@/components/ui/badge";

type ViewMode = "painted" | "original" | "split";

interface PreviewProps {
  original: ImageData | null;
  painted: ImageData | null;
  elapsedMs: number | null;
  busy: boolean;
}

function drawTo(canvas: HTMLCanvasElement | null, image: ImageData | null) {
  if (!canvas || !image) return;
  canvas.width = image.width;
  canvas.height = image.height;
  canvas.getContext("2d")?.putImageData(image, 0, 0);
}

export function Preview({ original, painted, elapsedMs, busy }: PreviewProps) {
  const [mode, setMode] = useState<ViewMode>("split");
  const [split, setSplit] = useState(50);
  const originalRef = useRef<HTMLCanvasElement>(null);
  const paintedRef = useRef<HTMLCanvasElement>(null);
  const containerRef = useRef<HTMLDivElement>(null);

  useEffect(() => drawTo(originalRef.current, original), [original, mode]);
  useEffect(() => drawTo(paintedRef.current, painted), [painted, mode]);

  const onDrag = (ev: React.PointerEvent) => {
    if (mode !== "split" || ev.buttons !== 1) return;
    const rect = containerRef.current?.getBoundingClientRect();
    if (!rect) return;
    setSplit(
      Math.min(100, Math.max(0, ((ev.clientX - rect.left) / rect.width) * 100)),
    );
  };

  return (
    <div className="flex h-full flex-col gap-2">
      <div className="flex items-center justify-between">
        <Tabs value={mode} onValueChange={(v) => setMode(v as ViewMode)}>
          <TabsList>
            <TabsTrigger value="split">Split</TabsTrigger>
            <TabsTrigger value="painted">Painted</TabsTrigger>
            <TabsTrigger value="original">Original</TabsTrigger>
          </TabsList>
        </Tabs>
        <div className="flex items-center gap-2">
          {busy && <Badge variant="secondary">computing…</Badge>}
          {elapsedMs !== null && !busy && (
            <Badge variant="outline" className="font-mono tabular-nums">
              {elapsedMs.toFixed(0)} ms
            </Badge>
          )}
        </div>
      </div>

      <div
        ref={containerRef}
        className="relative min-h-0 flex-1 touch-none overflow-hidden rounded-lg border bg-muted/30"
        onPointerDown={onDrag}
        onPointerMove={onDrag}
      >
        {mode !== "painted" && (
          <canvas
            ref={originalRef}
            className="absolute inset-0 h-full w-full object-contain"
          />
        )}
        {mode !== "original" && (
          <canvas
            ref={paintedRef}
            className="absolute inset-0 h-full w-full object-contain"
            style={
              mode === "split"
                ? { clipPath: `inset(0 0 0 ${split}%)` }
                : undefined
            }
          />
        )}
        {mode === "split" && painted && (
          <div
            className="pointer-events-none absolute inset-y-0 w-0.5 bg-primary/70"
            style={{ left: `${split}%` }}
          />
        )}
        {!original && (
          <div className="absolute inset-0 flex items-center justify-center text-sm text-muted-foreground">
            Upload an image to begin
          </div>
        )}
      </div>
    </div>
  );
}
