import { Label } from "@/components/ui/label";
import { Slider } from "@/components/ui/slider";

interface ParamSliderProps {
  label: string;
  value: number;
  min: number;
  max: number;
  step?: number;
  unit?: string;
  onChange: (value: number) => void;
}

export function ParamSlider({
  label,
  value,
  min,
  max,
  step = 1,
  unit = "",
  onChange,
}: ParamSliderProps) {
  return (
    <div className="space-y-1.5">
      <div className="flex items-center justify-between">
        <Label className="text-xs">{label}</Label>
        <span className="font-mono text-xs text-muted-foreground tabular-nums">
          {step < 1 ? value.toFixed(2) : value}
          {unit}
        </span>
      </div>
      <Slider
        value={[value]}
        min={min}
        max={max}
        step={step}
        onValueChange={([v]) => onChange(v)}
      />
    </div>
  );
}
