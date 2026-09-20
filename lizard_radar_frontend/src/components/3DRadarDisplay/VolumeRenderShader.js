import { Vector2, Vector3 } from "three";

const VolumeRenderShader = {
  uniforms: {
    u_size: { value: new Vector3(1, 1, 1) },
    u_renderstyle: { value: 0 },
    u_renderthreshold: { value: 0.5 },
    u_raystop: { value: 0.95 },
    u_clim: { value: new Vector2(0, 1) },
    u_data: { value: null },
    u_cmdata: { value: null },
    u_opacity: { value: 1 },
  },

  vertexShader: /* glsl */ `
    varying vec3 v_position;
    varying vec3 v_camera_in_object;

    void main() {
      vec4 position4 = vec4(position, 1.0);
      v_position = position;
      v_camera_in_object = (inverse(modelMatrix) * vec4(cameraPosition, 1.0)).xyz;
      gl_Position = projectionMatrix * modelViewMatrix * position4;
    }
  `,

  fragmentShader: /* glsl */ `
    precision highp float;
    precision mediump sampler3D;

    uniform vec3 u_size;
    uniform int u_renderstyle;
    uniform float u_renderthreshold;
    uniform float u_raystop;
    uniform vec2 u_clim;
    uniform sampler3D u_data;
    uniform sampler2D u_cmdata;
    uniform float u_opacity;

    varying vec3 v_position;
    varying vec3 v_camera_in_object;

    const int MAX_STEPS = 887;
    const int REFINEMENT_STEPS = 4;
    const float relative_step_size = 1.0;
    const vec4 ambient_light = vec4(0.18, 0.22, 0.3, 1.0);
    const vec4 specular_light = vec4(0.75, 0.9, 1.0, 1.0);
    const float shininess = 40.0;
    const float OPACITY_PER_SAMPLE = 0.3;

    void cast_raymarch(vec3 start_loc, vec3 step, int nsteps, vec3 view_ray);
    void cast_iso(vec3 start_loc, vec3 step, int nsteps, vec3 view_ray);
    float sample1(vec3 texcoords);
    vec4 apply_colormap(float val);
    vec4 add_lighting(float val, vec3 loc, vec3 step, vec3 view_ray);

    void main() {
      vec3 view_ray = normalize(v_camera_in_object - v_position);
      vec3 t1 = (vec3(-0.5) - v_position) / view_ray;
      vec3 t2 = (u_size - vec3(0.5) - v_position) / view_ray;
      vec3 tmax = max(t1, t2);
      float distance = min(min(tmax.x, tmax.y), tmax.z);

      int nsteps = int(distance / relative_step_size + 0.5);
      if (nsteps < 1) discard;

      vec3 front = v_position + view_ray * distance;
      vec3 step = ((v_position - front) / u_size) / float(nsteps);
      vec3 start_loc = (front + vec3(0.5)) / u_size;

      if (u_renderstyle == 0) cast_raymarch(start_loc, step, nsteps, view_ray);
      else if (u_renderstyle == 1) cast_iso(start_loc, step, nsteps, view_ray);

      if (gl_FragColor.a < 0.05) discard;
    }

    float sample1(vec3 texcoords) {
      return texture(u_data, clamp(texcoords.xyz, vec3(0.0), vec3(1.0))).r;
    }

    vec4 apply_colormap(float val) {
      val = clamp((val - u_clim[0]) / (u_clim[1] - u_clim[0]), 0.0, 1.0);
      float color_count = float(textureSize(u_cmdata, 0).x);
      val = (val * (color_count - 1.0) + 0.5) / color_count;
      return texture2D(u_cmdata, vec2(val, 0.5));
    }

    void cast_raymarch(vec3 start_loc, vec3 step, int nsteps, vec3 view_ray) {
      vec4 accumulated = vec4(0.0);
      vec3 loc = start_loc + 0.5 * step;

      for (int iter = 0; iter < MAX_STEPS; iter++) {
        if (iter >= nsteps || accumulated.a >= u_raystop) break;
        if (any(lessThan(loc, vec3(-0.0001))) || any(greaterThan(loc, vec3(1.0001)))) break;
        float val = sample1(loc);
        float intensity = clamp((val - u_clim[0]) / max(u_clim[1] - u_clim[0], 0.00001), 0.0, 1.0);
        vec4 sample_color = apply_colormap(val);
        float sample_alpha = sample_color.a * intensity * u_opacity * OPACITY_PER_SAMPLE;
        accumulated.rgb += (1.0 - accumulated.a) * sample_color.rgb * sample_alpha;
        accumulated.a += (1.0 - accumulated.a) * sample_alpha;
        loc += step;
      }

      accumulated.rgb /= max(accumulated.a, 0.00001);
      gl_FragColor = accumulated;
    }

    void cast_iso(vec3 start_loc, vec3 step, int nsteps, vec3 view_ray) {
      gl_FragColor = vec4(0.0);
      vec3 dstep = 1.5 / u_size;
      vec3 loc = start_loc;
      float low_threshold = u_renderthreshold - 0.02 * (u_clim[1] - u_clim[0]);

      for (int iter = 0; iter < MAX_STEPS; iter++) {
        if (iter >= nsteps) break;
        if (any(lessThan(loc, vec3(-0.0001))) || any(greaterThan(loc, vec3(1.0001)))) break;
        float val = sample1(loc);
        if (val > low_threshold) {
          vec3 iloc = loc - 0.5 * step;
          vec3 istep = step / float(REFINEMENT_STEPS);
          for (int i = 0; i < REFINEMENT_STEPS; i++) {
            val = sample1(iloc);
            if (val > u_renderthreshold) {
              gl_FragColor = add_lighting(val, iloc, dstep, view_ray);
              return;
            }
            iloc += istep;
          }
        }
        loc += step;
      }
    }

    vec4 add_lighting(float val, vec3 loc, vec3 step, vec3 view_ray) {
      vec3 V = normalize(view_ray);
      vec3 N;
      float val1;
      float val2;

      val1 = sample1(loc + vec3(-step[0], 0.0, 0.0));
      val2 = sample1(loc + vec3(+step[0], 0.0, 0.0));
      N[0] = val1 - val2;
      val = max(max(val1, val2), val);
      val1 = sample1(loc + vec3(0.0, -step[1], 0.0));
      val2 = sample1(loc + vec3(0.0, +step[1], 0.0));
      N[1] = val1 - val2;
      val = max(max(val1, val2), val);
      val1 = sample1(loc + vec3(0.0, 0.0, -step[2]));
      val2 = sample1(loc + vec3(0.0, 0.0, +step[2]));
      N[2] = val1 - val2;
      val = max(max(val1, val2), val);

      float gradient_length = length(N);
      N = gradient_length > 0.00001 ? N / gradient_length : -V;
      float Nselect = float(dot(N, V) > 0.0);
      N = (2.0 * Nselect - 1.0) * N;
      vec3 L = normalize(view_ray);
      float lambertTerm = clamp(dot(N, L), 0.0, 1.0);
      vec3 H = normalize(L + V);
      float specularTerm = pow(max(dot(H, N), 0.0), shininess);

      vec4 color = apply_colormap(val);
      vec4 final_color = color * (ambient_light + vec4(vec3(lambertTerm), 1.0))
        + specularTerm * specular_light;
      final_color.a = color.a * u_opacity;
      return final_color;
    }
  `,
};

export { VolumeRenderShader };
