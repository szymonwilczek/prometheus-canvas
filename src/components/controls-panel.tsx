import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";
import { Label } from "@/components/ui/label";
import { Button } from "@/components/ui/button";
import { Switch } from "@/components/ui/switch";
import { ParamSlider } from "@/components/param-slider";
import { PRESETS } from "@/engine/presets";
import {
  DEFAULT_OUTPUT_PARAMS,
  DEFAULT_PAINT_PARAMS,
  type OutputParams,
  type PaintParams,
} from "@/engine/types";

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

  const activePreset =
    PRESETS.find((p) => JSON.stringify(p.params) === JSON.stringify(params))
      ?.id ?? "";

  return (
    <div className="flex flex-col gap-3">
      <div className="flex items-center justify-between gap-2">
        <Label className="text-xs">Preset</Label>
        <Select
          value={activePreset}
          onValueChange={(id) => {
            const preset = PRESETS.find((p) => p.id === id);
            if (preset) onParams({ ...preset.params });
          }}
        >
          <SelectTrigger size="sm" className="w-40">
            <SelectValue placeholder="Custom" />
          </SelectTrigger>
          <SelectContent>
            {PRESETS.map((p) => (
              <SelectItem key={p.id} value={p.id}>
                {p.name}
              </SelectItem>
            ))}
          </SelectContent>
        </Select>
      </div>

      <Button
        variant="outline"
        size="sm"
        className="w-full"
        onClick={() => {
          onParams({ ...DEFAULT_PAINT_PARAMS });
          onOutput({ ...DEFAULT_OUTPUT_PARAMS });
        }}
      >
        Reset to defaults
      </Button>
      <Card size="sm">
        <CardHeader>
          <CardTitle className="text-sm">Renderer</CardTitle>
        </CardHeader>
        <CardContent className="space-y-3">
          <div className="flex items-center justify-between gap-2">
            <Label className="text-xs">Mode</Label>
            <Select
              value={params.mode}
              onValueChange={(v) => set("mode", v as PaintParams["mode"])}
            >
              <SelectTrigger size="sm" className="w-40">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                <SelectItem value="sbr">Impasto oil (SBR)</SelectItem>
                <SelectItem value="knife">Palette knife</SelectItem>
                <SelectItem value="brush">Brush (legacy LIC)</SelectItem>
              </SelectContent>
            </Select>
          </div>
          <ParamSlider
            label="Smoothing radius"
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
          {params.mode === "brush" && (
            <ParamSlider
              label="Stroke length (flow)"
              value={params.strokeLength}
              min={0}
              max={20}
              unit="px"
              onChange={(v) => set("strokeLength", v)}
            />
          )}
          {params.mode === "knife" && (
            <>
              <ParamSlider
                label="Knife size"
                value={params.knifeSize}
                min={12}
                max={72}
                unit="px"
                onChange={(v) => set("knifeSize", v)}
              />
              <ParamSlider
                label="Knife detail"
                value={params.knifeDetail}
                min={0}
                max={1}
                step={0.05}
                onChange={(v) => set("knifeDetail", v)}
              />
              <ParamSlider
                label="Blade edge ridges"
                value={params.knifeRidge}
                min={0}
                max={1}
                step={0.05}
                onChange={(v) => set("knifeRidge", v)}
              />
            </>
          )}
          {params.mode !== "brush" && (
            <>
              <ParamSlider
                label="Paint dryness (skipping)"
                value={params.knifeDryness}
                min={0}
                max={1}
                step={0.05}
                onChange={(v) => set("knifeDryness", v)}
              />
              <ParamSlider
                label="Pigment dragging"
                value={params.knifeDrag}
                min={0}
                max={1}
                step={0.05}
                onChange={(v) => set("knifeDrag", v)}
              />
              <ParamSlider
                label="Pigment mix vibrancy"
                value={params.paintVibrancy}
                min={0}
                max={1}
                step={0.05}
                onChange={(v) => set("paintVibrancy", v)}
              />
              <ParamSlider
                label="Edge fringing"
                value={params.edgeFringe}
                min={0}
                max={1}
                step={0.05}
                onChange={(v) => set("edgeFringe", v)}
              />
            </>
          )}
          {params.mode === "sbr" && (
            <>
              <ParamSlider
                label="Base stroke size"
                value={params.knifeSize}
                min={12}
                max={72}
                unit="px"
                onChange={(v) => set("knifeSize", v)}
              />
              <ParamSlider
                label="Vector alignment"
                value={params.sbrAlignment}
                min={0}
                max={1}
                step={0.05}
                onChange={(v) => set("sbrAlignment", v)}
              />
            </>
          )}
        </CardContent>
      </Card>

      {params.mode === "sbr" && (
        <Card size="sm">
          <CardHeader>
            <CardTitle className="text-sm">Paint layers -- SBR</CardTitle>
          </CardHeader>
          <CardContent className="space-y-3">
            <div className="flex items-center justify-between gap-2">
              <Label className="text-xs">Undercoat layer</Label>
              <Switch
                checked={params.sbrUndercoat}
                onCheckedChange={(v) => set("sbrUndercoat", v)}
              />
            </div>
            {params.sbrUndercoat && (
              <ParamSlider
                label="Undercoat strokes"
                value={params.sbrUndercoatDensity}
                min={0.25}
                max={2}
                step={0.05}
                unit="×"
                onChange={(v) => set("sbrUndercoatDensity", v)}
              />
            )}
            <div className="flex items-center justify-between gap-2">
              <Label className="text-xs">Form layer</Label>
              <Switch
                checked={params.sbrForm}
                onCheckedChange={(v) => set("sbrForm", v)}
              />
            </div>
            {params.sbrForm && (
              <ParamSlider
                label="Form strokes"
                value={params.sbrFormDensity}
                min={0.25}
                max={2}
                step={0.05}
                unit="×"
                onChange={(v) => set("sbrFormDensity", v)}
              />
            )}
            <div className="flex items-center justify-between gap-2">
              <Label className="text-xs">Detail layer</Label>
              <Switch
                checked={params.sbrDetail}
                onCheckedChange={(v) => set("sbrDetail", v)}
              />
            </div>
            {params.sbrDetail && (
              <ParamSlider
                label="Detail strokes"
                value={params.sbrDetailDensity}
                min={0.25}
                max={2}
                step={0.05}
                unit="×"
                onChange={(v) => set("sbrDetailDensity", v)}
              />
            )}
          </CardContent>
        </Card>
      )}

      <Card size="sm">
        <CardHeader>
          <CardTitle className="text-sm">Pigments -- k-means</CardTitle>
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
          <ParamSlider
            label="Pigment mixing noise"
            value={params.pigmentNoise}
            min={0}
            max={1}
            step={0.05}
            onChange={(v) => set("pigmentNoise", v)}
          />
          <ParamSlider
            label="Noise patch size"
            value={params.noiseScale}
            min={2}
            max={24}
            unit="px"
            onChange={(v) => set("noiseScale", v)}
          />
        </CardContent>
      </Card>

      <Card size="sm">
        <CardHeader>
          <CardTitle className="text-sm">Impasto -- Blinn-Phong</CardTitle>
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
          {params.mode !== "brush" && (
            <ParamSlider
              label="Brush anisotropy"
              value={params.brushAnisotropy}
              min={0}
              max={1}
              step={0.05}
              onChange={(v) => set("brushAnisotropy", v)}
            />
          )}
          <ParamSlider
            label="Cavity shadows"
            value={params.cavity}
            min={0}
            max={1}
            step={0.05}
            onChange={(v) => set("cavity", v)}
          />
          {params.mode !== "knife" && (
            <ParamSlider
              label={
                params.mode === "sbr" ? "Bristle intensity" : "Bristle grooves"
              }
              value={params.bristle}
              min={0}
              max={1}
              step={0.05}
              onChange={(v) => set("bristle", v)}
            />
          )}
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
          <CardTitle className="text-sm">Varnish -- refraction</CardTitle>
        </CardHeader>
        <CardContent className="space-y-3">
          <ParamSlider
            label="Layer thickness (0 = off)"
            value={params.varnishThickness}
            min={0}
            max={1}
            step={0.05}
            onChange={(v) => set("varnishThickness", v)}
          />
          <ParamSlider
            label="Refractive index (n)"
            value={params.varnishIor}
            min={1}
            max={1.8}
            step={0.01}
            onChange={(v) => set("varnishIor", v)}
          />
          <ParamSlider
            label="Gloss map dependency"
            value={params.glossDependency}
            min={0}
            max={1}
            step={0.05}
            onChange={(v) => set("glossDependency", v)}
          />
        </CardContent>
      </Card>

      <Card size="sm">
        <CardHeader>
          <CardTitle className="text-sm">Subsurface -- BSSRDF dipole</CardTitle>
        </CardHeader>
        <CardContent className="space-y-3">
          <ParamSlider
            label="Scattering σs′ (0 = off)"
            value={params.sssScatter}
            min={0}
            max={2}
            step={0.05}
            onChange={(v) => set("sssScatter", v)}
          />
          <ParamSlider
            label="Absorption σa"
            value={params.sssAbsorb}
            min={0.01}
            max={1}
            step={0.01}
            onChange={(v) => set("sssAbsorb", v)}
          />
        </CardContent>
      </Card>

      <Card size="sm">
        <CardHeader>
          <CardTitle className="text-sm">Craquelure -- fracture</CardTitle>
        </CardHeader>
        <CardContent className="space-y-3">
          <ParamSlider
            label="Drying tension (0 = off)"
            value={params.crackTension}
            min={0}
            max={1}
            step={0.05}
            onChange={(v) => set("crackTension", v)}
          />
          <ParamSlider
            label="V-groove depth"
            value={params.crackDepth}
            min={0}
            max={1}
            step={0.05}
            onChange={(v) => set("crackDepth", v)}
          />
          <ParamSlider
            label="Age / dirt in cracks"
            value={params.crackAge}
            min={0}
            max={1}
            step={0.05}
            onChange={(v) => set("crackAge", v)}
          />
        </CardContent>
      </Card>

      <Card size="sm">
        <CardHeader>
          <CardTitle className="text-sm">
            Canvas stretch -- elasticity
          </CardTitle>
        </CardHeader>
        <CardContent className="space-y-3">
          <ParamSlider
            label="Tension force (0 = off)"
            value={params.warpTension}
            min={0}
            max={1}
            step={0.05}
            onChange={(v) => set("warpTension", v)}
          />
          <ParamSlider
            label="Poisson's ratio (ν)"
            value={params.warpPoisson}
            min={0}
            max={0.45}
            step={0.01}
            onChange={(v) => set("warpPoisson", v)}
          />
          <ParamSlider
            label="Corner wrinkle frequency"
            value={params.wrinkleFrequency}
            min={1}
            max={12}
            step={1}
            onChange={(v) => set("wrinkleFrequency", v)}
          />
        </CardContent>
      </Card>

      <Card size="sm">
        <CardHeader>
          <CardTitle className="text-sm">Output -- Lanczos3</CardTitle>
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
