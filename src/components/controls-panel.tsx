import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";
import { Label } from "@/components/ui/label";
import { ParamSlider } from "@/components/param-slider";
import type { OutputParams, PaintParams } from "@/engine/types";

interface ControlsPanelProps {
  params: PaintParams;
  output: OutputParams;
  onParams: (p: PaintParams) => void;
  onOutput: (o: OutputParams) => void;
}

export function ControlsPanel({
  params,
  output,
  onParams,
  onOutput,
}: ControlsPanelProps) {
  const set = <K extends keyof PaintParams>(k: K, v: PaintParams[K]) =>
    onParams({ ...params, [k]: v });
  const setOut = <K extends keyof OutputParams>(k: K, v: OutputParams[K]) =>
    onOutput({ ...output, [k]: v });

  return (
    <div className="flex flex-col gap-3">
      <Card size="sm">
        <CardHeader>
          <CardTitle className="text-sm">Brush — Kuwahara</CardTitle>
        </CardHeader>
        <CardContent className="space-y-3">
          <ParamSlider
            label="Brush radius"
            value={params.kuwaharaRadius}
            min={0}
            max={30}
            unit="px"
            onChange={(v) => set("kuwaharaRadius", v)}
          />
          <ParamSlider
            label="Edge preservation (q)"
            value={params.edgeQ}
            min={0.5}
            max={16}
            step={0.5}
            onChange={(v) => set("edgeQ", v)}
          />
          <ParamSlider
            label="Stroke length (flow)"
            value={params.strokeLength}
            min={0}
            max={20}
            unit="px"
            onChange={(v) => set("strokeLength", v)}
          />
        </CardContent>
      </Card>

      <Card size="sm">
        <CardHeader>
          <CardTitle className="text-sm">Pigments — k-means</CardTitle>
        </CardHeader>
        <CardContent className="space-y-3">
          <ParamSlider
            label="Pigment count (0 = off)"
            value={params.pigments}
            min={0}
            max={64}
            onChange={(v) => set("pigments", v)}
          />
          <ParamSlider
            label="Saturation"
            value={params.saturation}
            min={0}
            max={2}
            step={0.05}
            onChange={(v) => set("saturation", v)}
          />
          <ParamSlider
            label="Contrast"
            value={params.contrast}
            min={0.5}
            max={1.6}
            step={0.05}
            onChange={(v) => set("contrast", v)}
          />
        </CardContent>
      </Card>

      <Card size="sm">
        <CardHeader>
          <CardTitle className="text-sm">Impasto — Blinn-Phong</CardTitle>
        </CardHeader>
        <CardContent className="space-y-3">
          <ParamSlider
            label="Texture depth"
            value={params.impastoDepth}
            min={0}
            max={100}
            onChange={(v) => set("impastoDepth", v)}
          />
          <ParamSlider
            label="Light elevation"
            value={params.lightElevation}
            min={5}
            max={90}
            unit="°"
            onChange={(v) => set("lightElevation", v)}
          />
          <ParamSlider
            label="Light azimuth"
            value={params.lightAzimuth}
            min={0}
            max={360}
            unit="°"
            onChange={(v) => set("lightAzimuth", v)}
          />
          <ParamSlider
            label="Specular"
            value={params.specular}
            min={0}
            max={1}
            step={0.05}
            onChange={(v) => set("specular", v)}
          />
          <ParamSlider
            label="Shininess"
            value={params.shininess}
            min={2}
            max={128}
            step={2}
            onChange={(v) => set("shininess", v)}
          />
          <ParamSlider
            label="Bristle grooves"
            value={params.bristle}
            min={0}
            max={1}
            step={0.05}
            onChange={(v) => set("bristle", v)}
          />
          <ParamSlider
            label="Canvas weave"
            value={params.canvasWeave}
            min={0}
            max={1}
            step={0.05}
            onChange={(v) => set("canvasWeave", v)}
          />
          <ParamSlider
            label="Weave pitch"
            value={params.weaveScale}
            min={2}
            max={12}
            unit="px"
            onChange={(v) => set("weaveScale", v)}
          />
        </CardContent>
      </Card>

      <Card size="sm">
        <CardHeader>
          <CardTitle className="text-sm">Output — Lanczos3</CardTitle>
        </CardHeader>
        <CardContent className="space-y-3">
          <div className="flex items-center justify-between gap-2">
            <Label className="text-xs">Upscale factor</Label>
            <Select
              value={String(output.upscaleFactor)}
              onValueChange={(v) => setOut("upscaleFactor", Number(v))}
            >
              <SelectTrigger size="sm" className="w-24">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                <SelectItem value="1">1x</SelectItem>
                <SelectItem value="2">2x</SelectItem>
                <SelectItem value="4">4x</SelectItem>
              </SelectContent>
            </Select>
          </div>
          <ParamSlider
            label="Unsharp amount"
            value={output.unsharpAmount}
            min={0}
            max={2}
            step={0.05}
            onChange={(v) => setOut("unsharpAmount", v)}
          />
          <ParamSlider
            label="Unsharp sigma"
            value={output.unsharpSigma}
            min={0.5}
            max={4}
            step={0.1}
            onChange={(v) => setOut("unsharpSigma", v)}
          />
        </CardContent>
      </Card>
    </div>
  );
}
