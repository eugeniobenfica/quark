# A fazer

Só o que falta. O que já foi feito, o que foi tentado e deixado de fora (e por quê), com as medições, está no
[PLANO.md](PLANO.md); a história das versões, no [CHANGELOG.md](../CHANGELOG.md).

`[~]` começado · `[ ]` a fazer

Estado em 21/11/2026: a versão publicada é a 0.12.0 (a CI parte dela); o `main` tem as fases A a E do plano e a
revisão para o congelamento (a std, o prelúdio, a API C e a sintaxe), com a CI verde em Windows, Linux x86-64 e
ARM64, macOS (x86-64 e ARM64) e WebAssembly, o compilador se reconstruindo nos mesmos bytes e a suíte inteira
passando.

## Em ordem

1. [~] **Fechar o marco 0.12: o rebuild do compilador abaixo de 0,5 s** (hoje ~1,75 s): a B13 no
   [PLANO.md](PLANO.md), em etapas
   - [x] 1. o código de uma função reaproveitável: as linhas como remendos que se movem (`rebased`), uma parte por
     função (`splitPart`); provado na suíte (as referências por símbolo caíram: partindo da imagem, os números ficam)
   - [x] 3a. o programa checado numa imagem, lido de volta quando nenhum arquivo mudou (ler e checar: 550 → 17 ms)
   - [~] 2. o build incremental sobre a imagem do build anterior: o build inteiro guardado nela (sem mudança, o
     rebuild em 65 ms); falta refazer só as funções que mudaram e as que as embutem
   - [ ] 3b. só os corpos que mudaram lidos e checados de novo, sobre a imagem
   - [ ] 4. as decisões de memória refeitas só para o que mudou
2. [~] **O fim da B12:**
   - [ ] no ARM64, a carga e o check numa tarefa esperam a próxima versão: as tarefas da 0.12.0 têm 8 MB de pilha,
     pouco para checar o compilador ali (o `main` já dá 64 MB a uma tarefa no Linux e no macOS);
   - [ ] o tempo de volta (a tarefa custa +5% de relógio): workers do `parallelMap` que vivem entre chamadas; um
     canal que entrega o valor sem copiá-lo.
3. [ ] **As ferramentas onde as pessoas estão.** Cada uma é pública: precisa do OK do dono.
   - uma extensão de editor (VS Code) que leve o `quark lsp` e o `quark dap`;
   - um registro de pacotes público (o repositório `index.toml`);
   - um playground no navegador (o compilador em wasm32);
   - instaladores por sistema.
4. [ ] **Desempenho, de novo medido nos três sistemas,** e o próximo alvo escolhido pelo que estiver mais longe do C.
   No Windows, em 21/11/2026 (`bash tools/bench.sh`): records 1,44×, mandelbrot 1,06×, counting 1,04×, words 1,00×;
   mais rápidos que o C: sorting 0,77×, strings 0,64×, pipeline 0,42×, trees 0,19×.
5. [ ] **1.0:** o STABILITY.md revisto uma última vez, o changelog, o anúncio.

## Sem data

- [ ] **A6: assinar o `quark.exe` no Windows.** Depende de um certificado de assinatura de código (Azure Trusted
  Signing, OV ou EV), que só o dono do projeto pode tirar. Sem ele, o Smart App Control e o SmartScreen barram
  executáveis novos.
- [ ] **SIMD fora do `--release`:** os laços só são vetorizados no tier otimizador.
