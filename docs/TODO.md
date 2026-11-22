# A fazer

Só o que falta. O que já foi feito, o que foi tentado e deixado de fora (e por quê), com as medições, está no
[PLANO.md](PLANO.md); a história das versões, no [CHANGELOG.md](../CHANGELOG.md).

`[~]` começado · `[ ]` a fazer

Estado em 22/11/2026: a versão publicada é a 0.12.1 (a CI parte dela), e o marco 0.12 está fechado. O `main` tem as
fases A a E do plano, a revisão para o congelamento e o rebuild incremental (B13), com a CI verde em Windows, Linux
x86-64 e ARM64, macOS (x86-64 e ARM64) e WebAssembly, o compilador se reconstruindo nos mesmos bytes e a suíte
inteira passando.

## Em ordem

1. [ ] **As ferramentas onde as pessoas estão.** Cada uma é pública: precisa do OK do dono.
   - uma extensão de editor (VS Code) que leve o `quark lsp` e o `quark dap`;
   - um registro de pacotes público (o repositório `index.toml`);
   - um playground no navegador (o compilador em wasm32);
   - instaladores por sistema.
2. [ ] **Desempenho: o próximo alvo**, escolhido pelo que está mais longe do C nos três sistemas. Medido em 22/11/2026
   (`bash tools/bench.sh 5` no Windows; o workflow `bench.yml`, à mão, nas máquinas da CI: tempo de Quark / tempo do C):

   | | Windows | Linux x86-64 | Linux ARM64 | macOS Intel | macOS Apple |
   |---|---|---|---|---|---|
   | records | 1,46 | 1,58 | 1,69 | 1,80 | 1,34 |
   | counting | 1,04 | 1,68 | 1,40 | 1,53 | 1,86 |
   | sorting | 0,74 | 1,66 | 1,12 | 0,61 | 0,96 |
   | strings | 0,61 | 1,40 | 1,17 | 1,10 | 0,93 |
   | words | 1,02 | 1,48 | 1,17 | 0,75 | 0,78 |
   | pipeline | 0,42 | 0,39 | 1,31 | 0,86 | 1,45 |
   | mandelbrot | 1,03 | 1,08 | 1,12 | 1,03 | 1,02 |
   | trees | 0,19 | 0,33 | 0,48 | 0,18 | 0,31 |

   **records** (registros num array) passa de 1,3× em todos os sistemas, e **counting** (um mapa de contagem) em
   todos fora do Windows; sorting, strings e words ficam acima de 1,4× no Linux x86-64. (Máquinas compartilhadas: a
   razão vale mais que o tempo.)
   - **records:** a distância é toda das coletas dos laços (a primeira cópia do que sobrevive). O tenure na coleta
     foi tentado de novo em 22/11 e desfeito (`json` mais lento no Linux, PLANO B7). O próximo passo é a
     pré-promoção por lugar, com cada saída de laço marcada no lowering.
3. [ ] **1.0:** o STABILITY.md revisto uma última vez, o changelog, o anúncio.

## Sem data

- [ ] **A6: assinar o `quark.exe` no Windows.** Depende de um certificado de assinatura de código (Azure Trusted
  Signing, OV ou EV), que só o dono do projeto pode tirar. Sem ele, o Smart App Control e o SmartScreen barram
  executáveis novos.
- [ ] **SIMD fora do `--release`:** os laços só são vetorizados no tier otimizador.
