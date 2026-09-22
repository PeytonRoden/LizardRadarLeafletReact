// Pre-rendered GIF of the つぶやきProcessing sketch:
// a=(y,d=mag(k=8*cos(y<59?i%9:i*4),e=y/7-16)**2/89+1.1)=>point((q=k/d*3-e*sin(k)+k/d/3*(9-d*4*sin(d*d-t+sin(e)/2))+70)*sin(c=d/1.5-t/12+i%2*3)+200,q*cos(c)+e+200)
// t=0,draw=$=>{t||createCanvas(w=400,w);background(9).stroke(w,96);for(t+=PI/60,i=2e4;i--;)a(i/99)}
// A GIF keeps animating even while the synchronous WASM voxel build blocks
// the main thread, which a rAF-driven canvas cannot do.
export default function LoadingSketch({ className = "" }) {
  return (
    <div className={`loading-sketch ${className}`.trim()} aria-hidden="true">
      <img src="/loading-sketch.gif" alt="" />
    </div>
  );
}
