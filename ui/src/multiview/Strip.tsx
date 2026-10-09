import type { CameraConfig, CameraState } from '../api/protocol';
import { Icon } from '../components/Icon';
import { inputCode, inputName, outputCode, outputName, signalLabel } from '../state/labels';
import { clock, holdLeft, type Marks } from '../state/marks';

// The one-row state strip (docs/design/multiview.md): input chip · CAM n · name · → · outputs · mark.
export function Strip({ n, cfg, cam, marks, holdMs, now, dense, offline }: {
  n: number;
  cfg: CameraConfig;
  cam: CameraState | undefined;
  marks: Marks | undefined;
  holdMs: number;
  now: number;
  dense: boolean;
  offline: boolean;
}) {
  const health = offline ? 'offline' : cam?.input?.health ?? 'offline';
  const left = holdLeft(marks, holdMs, now);
  const slate = !offline && cam?.phase === 'slate';
  const holding = !offline && left !== null;
  const chipCls = slate || holding || health === 'down' ? 'err' : health === 'receiving' ? 'ready' : health === 'degraded' ? 'conn' : 'off';
  const name = inputName(cfg.input.kind);
  const chipTitle = slate
    ? `${name}: down, showing the slate`
    : holding
      ? `${name}: down, holding the last frame (${left} left)`
      : `${name}: ${health}`;
  const max = dense ? 2 : 4;
  const shown = cfg.outputs.slice(0, max);
  const stripCls = slate || holding ? ' err' : '';
  return (
    <div className={`strip${stripCls}`}>
      <span className={`in-chip ${chipCls}`} title={chipTitle}>
        {slate ? 'SLATE' : holding ? left : inputCode(cfg.input.kind)}
      </span>
      {!dense && <span className="cam-no">CAM {n}</span>}
      <span className="cam-name" title={cfg.name}>
        {cfg.name}
      </span>
      <span className="flow">
        <Icon name="arrow" size={12} />
      </span>
      <span className="outs">
        {shown.map((o) => {
          const os = cam?.outputs?.[o.id];
          const st = offline ? 'offline' : os?.state ?? 'ready';
          if (!offline && os?.error) {
            return (
              <span key={o.id} className="err-chip" title={`${outputName(o.kind)}: failed, ${os.error.message}`}>
                <Icon name="retry" size={11} />
                {outputCode(o.kind)}
              </span>
            );
          }
          return (
            <span key={o.id} className={`pill ${st}`} title={`${outputName(o.kind)}: ${signalLabel[st]}`}>
              <i />
              {outputCode(o.kind)}
            </span>
          );
        })}
        {cfg.outputs.length > max && (
          <span className="more" title={cfg.outputs.slice(max).map((o) => outputName(o.kind)).join(', ')}>
            +{cfg.outputs.length - max}
          </span>
        )}
      </span>
      {!offline && marks?.recoveredAt !== undefined && (
        <span className="seen" title="A fault recovered on its own. Select the camera to clear this mark.">
          <Icon name="seen" size={11} />
          {clock(marks.recoveredAt)}
        </span>
      )}
    </div>
  );
}
