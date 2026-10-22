// ScaleFX (Sp00kyFox, licencia MIT; shaders originales de libretro en web/scalefx/) en WebGL2.
// Cada archivo es un shader de RetroArch con su parte de vértices y de fragmentos; aquí se
// compilan tal cual como GLSL ES 3.0 dándoles lo que esperan: atributos VertexCoord/TexCoord,
// tamaños, parámetros y las salidas de pasadas anteriores (PassPrevN). Resultado: 3x.
(function () {
  const PARAMS = { SFX_CLR: 0.5, SFX_SAA: 1.0, SFX_SCN: 1.0 };

  function compile(gl, src) {
    const body = src.replace(/^#version.*$/m, '');
    const p = gl.createProgram();
    for (const [type, def] of [[gl.VERTEX_SHADER, 'VERTEX'], [gl.FRAGMENT_SHADER, 'FRAGMENT']]) {
      const s = gl.createShader(type);
      gl.shaderSource(s, `#version 300 es\n#define ${def}\n${body}`);
      gl.compileShader(s);
      if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s));
      gl.attachShader(p, s);
    }
    gl.bindAttribLocation(p, 0, 'VertexCoord');
    gl.bindAttribLocation(p, 1, 'TexCoord');
    gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p));
    return p;
  }

  async function loadScaleFX(gl) {
    if (!gl.getExtension('EXT_color_buffer_float')) throw new Error('sin texturas flotantes renderizables');
    const texts = await Promise.all([0, 1, 2, 3, 4].map((i) => fetch(`scalefx/scalefx-pass${i}.glsl`).then((r) => r.text())));
    const progs = texts.map((t) => compile(gl, t));

    // Coordenadas de textura para el atributo 1 (las del cuadrado de pantalla, sin invertir).
    const tc = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, tc);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([0, 0, 1, 0, 0, 1, 1, 1]), gl.STATIC_DRAW);

    const outs = progs.map(() => ({ tex: gl.createTexture(), fb: gl.createFramebuffer(), w: 0, h: 0 }));
    function ensure(o, w, h) {
      if (o.w === w && o.h === h) return;
      o.w = w; o.h = h;
      gl.bindTexture(gl.TEXTURE_2D, o.tex);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA16F, w, h, 0, gl.RGBA, gl.HALF_FLOAT, null);
      // Entre pasadas, sin interpolar; la última la lee el escalado final, que sí interpola.
      const f = o === outs[4] ? gl.LINEAR : gl.NEAREST;
      for (const [k, v] of [[gl.TEXTURE_MIN_FILTER, f], [gl.TEXTURE_MAG_FILTER, f],
        [gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE], [gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE]]) gl.texParameteri(gl.TEXTURE_2D, k, v);
      gl.bindFramebuffer(gl.FRAMEBUFFER, o.fb);
      gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, o.tex, 0);
    }
    const ident = new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);

    // `src` es la imagen original (filtrado NEAREST). Devuelve {tex, w, h} a 3x.
    return function run(src, w, h) {
      const history = [{ tex: src, w, h }];  // history[0] = original; luego cada pasada
      gl.bindBuffer(gl.ARRAY_BUFFER, tc);
      gl.enableVertexAttribArray(1);
      gl.vertexAttribPointer(1, 2, gl.FLOAT, false, 0, 0);
      progs.forEach((p, i) => {
        const prev = history[history.length - 1];
        const ow = i === 4 ? w * 3 : w, oh = i === 4 ? h * 3 : h;
        ensure(outs[i], ow, oh);
        gl.useProgram(p);
        gl.bindFramebuffer(gl.FRAMEBUFFER, outs[i].fb);
        gl.viewport(0, 0, ow, oh);
        const u = (n) => gl.getUniformLocation(p, n);
        gl.uniformMatrix4fv(u('MVPMatrix'), false, ident);
        gl.uniform2f(u('TextureSize'), prev.w, prev.h);
        gl.uniform2f(u('InputSize'), prev.w, prev.h);
        gl.uniform2f(u('OutputSize'), ow, oh);
        gl.uniform1i(u('FrameDirection'), 1);
        gl.uniform1i(u('FrameCount'), 0);
        for (const [k, v] of Object.entries(PARAMS)) gl.uniform1f(u(k), v);
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, prev.tex);
        gl.uniform1i(u('Texture'), 0);
        // PassPrevN = salida de N pasadas atrás (PassPrev5 en la última es el original).
        for (const n of [2, 5]) {
          const loc = u(`PassPrev${n}Texture`);
          if (!loc) continue;
          const t = history[history.length - n];
          gl.activeTexture(gl.TEXTURE1);
          gl.bindTexture(gl.TEXTURE_2D, t.tex);
          gl.uniform1i(loc, 1);
          gl.uniform2f(u(`PassPrev${n}TextureSize`), t.w, t.h);
          gl.uniform2f(u(`PassPrev${n}InputSize`), t.w, t.h);
        }
        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
        history.push({ tex: outs[i].tex, w: ow, h: oh });
      });
      gl.disableVertexAttribArray(1);
      gl.activeTexture(gl.TEXTURE0);
      return history[history.length - 1];
    };
  }

  window.X4ScaleFX = { loadScaleFX };
})();
