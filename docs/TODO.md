# A fazer

Só o que falta. O que já foi feito, o que foi tentado e deixado de fora (e por quê), com as medições, está no
[PLANO.md](PLANO.md); a história das versões, no [CHANGELOG.md](../CHANGELOG.md).

`[~]` começado · `[ ]` a fazer

Estado em 22/11/2026: a versão publicada é a 0.12.1 (a CI parte dela), e o marco 0.12 está fechado. O `main` tem as
fases A a E do plano, a revisão para o congelamento e o rebuild incremental (B13), com a CI verde em Windows, Linux
x86-64 e ARM64, macOS (x86-64 e ARM64) e WebAssembly, o compilador se reconstruindo nos mesmos bytes e a suíte
inteira passando.

## Em ordem

1. [ ] **O tempo de volta da B12** (a carga e o check numa tarefa custam +5% de relógio num build frio): workers do
   `parallelMap` que vivem entre chamadas; um canal que entrega o valor sem copiá-lo.
2. [ ] **As ferramentas onde as pessoas estão.** Cada uma é pública: precisa do OK do dono.
   - uma extensão de editor (VS Code) que leve o `quark lsp` e o `quark dap`;
   - um registro de pacotes público (o repositório `index.toml`);
   - um playground no navegador (o compilador em wasm32);
   - instaladores por sistema.
3. [ ] **Desempenho, de novo medido nos três sistemas,** e o próximo alvo escolhido pelo que estiver mais longe do C.
   No Windows, em 21/11/2026 (`bash tools/bench.sh`): records 1,44×, mandelbrot 1,06×, counting 1,04×, words 1,00×;
   mais rápidos que o C: sorting 0,77×, strings 0,64×, pipeline 0,42×, trees 0,19×.
4. [ ] **1.0:** o STABILITY.md revisto uma última vez, o changelog, o anúncio.

## Sem data

- [ ] **A6: assinar o `quark.exe` no Windows.** Depende de um certificado de assinatura de código (Azure Trusted
  Signing, OV ou EV), que só o dono do projeto pode tirar. Sem ele, o Smart App Control e o SmartScreen barram
  executáveis novos.
- [ ] **SIMD fora do `--release`:** os laços só são vetorizados no tier otimizador.
