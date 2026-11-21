# A fazer

Só o que falta. O que já foi feito, o que foi tentado e deixado de fora (e por quê), com as medições, está no
[PLANO.md](PLANO.md); a história das versões, no [CHANGELOG.md](../CHANGELOG.md).

`[~]` começado · `[ ]` a fazer

Estado em 21/11/2026: a versão publicada é a 0.9.2; o `main` tem as fases A a E do plano e a revisão para o
congelamento (a std, o prelúdio, a API C e a sintaxe), com a CI verde em Windows, Linux x86-64 e ARM64, macOS
(x86-64 e ARM64) e WebAssembly, o compilador se reconstruindo nos mesmos bytes e a suíte inteira passando.

## Em ordem

1. [ ] **Publicar a 0.12.** A CI ainda começa da 0.9.2, então o compilador não pode usar o que veio depois dela
   (canais tipados, `owned`). As notas estão prontas no [CHANGELOG.md](../CHANGELOG.md), com as renomeações da
   revisão. Precisa do OK do dono para publicar.
2. [~] **Fechar o marco 0.12: o rebuild do compilador abaixo de 0,5 s** (hoje ~1,7 s).
   - [ ] o cache por módulo: leitura, checagem e lowering guardados por módulo (o que leva de ~1,2 s a 0,5 s)
3. [~] **B12, a compilação em fluxo** (os lotes estão feitos: o pico do rebuild de 3,1 para 2,1 GB, o mesmo tempo)
   - [ ] a carga e o check numa tarefa (o lixo deles vai com ela: pico de 1,5 GB): feita e medida, espera a 0.12
     (o compilador que constrói este precisa de canais tipados)
   - [ ] o tempo de volta (a tarefa custa +5%): workers do `parallelMap` que vivem entre chamadas; um canal que
     entrega o valor sem copiá-lo
4. [ ] **As ferramentas onde as pessoas estão.** Cada uma é pública: precisa do OK do dono.
   - uma extensão de editor (VS Code) que leve o `quark lsp` e o `quark dap`;
   - um registro de pacotes público (o repositório `index.toml`);
   - um playground no navegador (o compilador em wasm32);
   - instaladores por sistema.
5. [ ] **Desempenho, de novo medido nos três sistemas,** e o próximo alvo escolhido pelo que estiver mais longe do C.
   No Windows, em 21/11/2026 (`bash tools/bench.sh`): records 1,44×, mandelbrot 1,06×, counting 1,04×, words 1,00×;
   mais rápidos que o C: sorting 0,77×, strings 0,64×, pipeline 0,42×, trees 0,19×.
6. [ ] **1.0:** o STABILITY.md revisto uma última vez, o changelog, o anúncio.

## Sem data

- [ ] **A6: assinar o `quark.exe` no Windows.** Depende de um certificado de assinatura de código (Azure Trusted
  Signing, OV ou EV), que só o dono do projeto pode tirar. Sem ele, o Smart App Control e o SmartScreen barram
  executáveis novos.
- [ ] **SIMD fora do `--release`:** os laços só são vetorizados no tier otimizador.
