import { useEffect, useRef } from "react";

const SIZE = 400;
const POINT_COUNT = 20000;

export default function GlobalLoadingIndicator({ label = "Loading" }) {
  const canvasRef = useRef(null);

  useEffect(() => {
    const canvas = canvasRef.current;
    const context = canvas.getContext("2d");
    const reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
    let animationFrame;
    let time = 0;

    const draw = () => {
      context.fillStyle = "rgb(9, 9, 9)";
      context.fillRect(0, 0, SIZE, SIZE);
      context.fillStyle = "rgba(255, 255, 255, 0.38)";

      for (let index = POINT_COUNT; index--;) {
        const y = index / 638;
        const k = (4 + Math.cos(y)) * Math.sin(index / 7) + 0.0001;
        const e = y / 6 - 9;
        const d = Math.hypot(k, e) - 3;
        const q = 99 + 3 * Math.sin(k * 3) - d * d * Math.sin(time - d)
          + y / 13 * k * (e + Math.sin(d * d - time * 3));
        const c = d / 3 - time / 4 + index % 2 * 9 + k * k / 59;
        context.fillRect(q * Math.sin(c) + 200, q * Math.cos(c) + 200, 1, 1);
      }

      if (!reduceMotion) {
        time += Math.PI / 60;
        animationFrame = requestAnimationFrame(draw);
      }
    };

    draw();
    return () => cancelAnimationFrame(animationFrame);
  }, []);

  return (
    <div className="global-loading" role="status" aria-live="polite">
      <canvas ref={canvasRef} width={SIZE} height={SIZE} aria-hidden="true" />
      <span>{label}</span>
    </div>
  );
}
