# A fazer

Onde está o [PLANO.md](PLANO.md): o que foi feito, o que foi tentado e deixado de fora (e por quê), o que falta. Os
detalhes, as medições e os motivos ficam no plano, sob cada item; esta lista só ajuda a achá-los.

`[x]` feito · `[~]` começado · `[ ]` a fazer · `[-]` medido ou pesado, e não feito (o plano diz por quê)

## A. Rede de segurança

- [x] A1 git: [github.com/eugeniobenfica/quark](https://github.com/eugeniobenfica/quark), público, MIT
- [x] A2 release (0.9.2) e CI rodando a suíte inteira em Windows, Linux x86-64 e ARM64, macOS
- [x] A3 teste diferencial (`tools/difftest.qk`; a cada mudança, 3000 programas toda noite)
- [x] A4 fuzzing (`tools/fuzz.qk`); `quark bind` ainda sem fuzzing
- [x] A5 as tabelas do runtime conferidas entre si
- [ ] A6 assinar o `quark.exe` no Windows: precisa de um certificado de assinatura de código (Azure Trusted Signing,
  OV/EV)
- [x] A7 forma canônica conferida na CI
- [x] `tools/parity.sh`: dois compiladores fazem o mesmo código (para refatorações)

## B. Estrutura

- [x] B1 built-ins de sistema declarados em Quark (`compiler/prelude.qk`)
- [x] B2 constantes por execução (`compiler/construn.qk`)
- [~] B3 um passo intermediário antes da IR de máquina
  - [-] passo 1 (desaçucarar): medido, pouco a ganhar
  - [x] as decisões de memória fora de `lower.qk`, em `compiler/ownership.qk`; o aliasing fundo calculado antes
  - [x] o último uso de um var entrega o valor (`decideTakes`)
  - [x] resultado todo seu dispensa cópia funda (`decideResults`)
  - [ ] as decisões ainda tomadas durante a emissão: self por endereço, a área da função, a coleta por volta de laço
  - [ ] análise de escape
- [x] B4 o heap contado removido
- [~] B5 o runtime inlinável
  - [x] alocação de tamanho conhecido no próprio código
  - [ ] as funções pequenas do runtime (comprimento, índice, push sem crescer, busca em mapa) coladas no código
- [-] B6 representação dos valores
  - [-] textos curtos dentro do valor: feitos em quatro etapas, medidos mais lentos (json +9%, words +16%), desfeitos
  - [-] opcionais sem etiqueta: um slot a menos, nenhum trabalho poupado em laços; não vale a mudança
  - [-] mapas em Swiss table: a tabela de índices com etiqueta já faz quase tudo o que ela faria
- [~] B7 copiar menos na coleta de laço
  - [x] a tabela de salas cresce quando perde salas demais (50.000 elementos com push: de 1.160 para 210 ms)
  - [x] um registro das salas: uma coleta olha só as feitas depois da sua marca
  - [-] pré-promoção por lugar: não alcança o caso de `records`
  - [-] promoção no lugar: `records` 15% mais rápido, mas superlinear no Linux (memória guardada): desfeita
  - [-] reutilização no lugar (`xs = xs + [x]` crescendo onde está): mais lenta que um array novo pequeno
  - [-] barreira de escrita filtrada no código: as chamadas fazem trabalho real, sem ganho
  - [ ] o que `records` precisa: saber ao alocar o que vai viver (uma região escolhida entre funções)
- [ ] B8 o grafo sem JSON por dentro
- [ ] B9 concorrência
  - [x] `Channel<T>` tipado (o `T` dito onde o valor vai: `let c: Channel<Int> = channel()`)
  - [x] tudo o que espera, cooperativo (`connect`, nomes, `runProcess`, o teclado: numa thread própria)
  - [ ] cancelamento e prazos
  - [ ] concorrência estruturada
- [ ] B10 compilação incremental de ponta a ponta

## C. A linguagem

- [x] constantes em padrões, `T?!`, `a..=b` decidido (não), `Char` decidido (um `Int`), só `defer`, efeitos na doc,
  `edition` lida
- [ ] o `?` convertendo erros (`E1` em `E2`)
- [ ] `Self` e implementações padrão em interfaces
- [ ] `pub` por campo

## D. Backends e sistema

- [x] escrever `Float` sem o C (dígitos mais curtos, exato)
- [x] ler `Float` sem o C (`strtod` não é mais usado)
- [ ] ARM64 em paridade (argumentos em registradores, NEON): precisa de uma máquina no ciclo
- [ ] informação de depuração DWARF e PDB
- [ ] a suíte em wasm32
- [ ] libquark no macOS, a API C revista antes de congelar
- [ ] executáveis menores (o runtime vai inteiro: 838 dos 842 KB de um hello)

## E. Biblioteca e ferramentas

- [x] `std.csv`, `walkFiles`, `quark test --filter`, o tutorial
- [x] `std.encoding`, `std.hash`, `std.args`, `std.files`, `std.net`, `std.http`
- [ ] TLS (por `quark bind` da biblioteca do sistema)
- [ ] JSON tipado (de e para records)
- [ ] subprocessos com entrada e saída em fluxo
- [ ] fusos horários com nome
- [ ] pacotes: registro, versões semânticas
- [ ] LSP: renomear, ir para implementações, tipos e efeitos ao passar o mouse
- [ ] testes: cobertura, testes de propriedade
- [ ] guias (vindo de Go, Rust, Python; servidores)

## Desempenho contra C (gcc -O2)

Mais rápido que C: pipeline, sorting, strings, trees. A até 7%: counting, mandelbrot, words. Atrás: records (2,0×: o
coletor copia o que sobrevive). `bash tools/bench.sh` mede.
