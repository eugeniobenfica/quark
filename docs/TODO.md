# A fazer

Onde está o [PLANO.md](PLANO.md): o que foi feito, o que foi tentado e deixado de fora (e por quê), o que falta. Os
detalhes, as medições e os motivos ficam no plano, sob cada item; esta lista só ajuda a achá-los.

`[x]` feito · `[~]` começado · `[ ]` a fazer · `[-]` medido ou pesado, e não feito (o plano diz por quê)

## A. Rede de segurança

- [x] A1 git: [github.com/eugeniobenfica/quark](https://github.com/eugeniobenfica/quark), público, MIT
- [x] A2 release (0.9.2) e CI rodando a suíte inteira em Windows, Linux x86-64 e ARM64, macOS
- [x] A3 teste diferencial (`tools/difftest.qk`; a cada mudança, 3000 programas toda noite)
- [x] A4 fuzzing (`tools/fuzz.qk`), `quark bind` também (um caso em quatro: um cabeçalho C mudado ao acaso)
- [x] A5 as tabelas do runtime conferidas entre si
- [ ] A6 assinar o `quark.exe` no Windows: precisa de um certificado de assinatura de código (Azure Trusted Signing,
  OV/EV)
- [x] A7 forma canônica conferida na CI
- [x] `tools/parity.sh`: dois compiladores fazem o mesmo código (para refatorações)

## B. Estrutura

- [x] B1 built-ins de sistema declarados em Quark (`compiler/prelude.qk`)
- [x] B2 constantes por execução (`compiler/construn.qk`)
- [x] B3 um passo intermediário antes da IR de máquina
  - [-] passo 1 (desaçucarar): medido, pouco a ganhar
  - [x] as decisões de memória fora de `lower.qk`, em `compiler/ownership.qk`; o aliasing fundo calculado antes
  - [x] o último uso de um var entrega o valor (`decideTakes`)
  - [x] resultado todo seu dispensa cópia funda (`decideResults`)
  - [x] as decisões que só leem a árvore fora da emissão (inlining, self por endereço); a checagem de fim de turno
    lê o IR de propósito
  - [x] último uso em ramos de `if` e `match`
  - [x] reutilização no lugar (`xs = xs.map(f)` num var)
  - [x] análise de escape: arrays literais que não saem da função ficam na área dela
- [x] B4 o heap contado removido
- [x] B5 o runtime inlinável
  - [x] alocação de tamanho conhecido no próprio código
  - [x] push, busca em mapa (chave simples no código, de texto numa função do programa), interpolação numa chamada
- [x] B6 representação dos valores
  - [-] textos curtos dentro do valor: feitos em quatro etapas, medidos mais lentos (json +9%, words +16%), desfeitos
  - [x] opcionais sem etiqueta (`Text?`, `Bytes?`, `[T]?`: `None` é o comprimento −1): `counting` −12%, `json` −2,5%
  - [-] mapas em Swiss table: feita e medida; faltas 2× mais rápidas, acertos 18% mais lentos: desfeita
- [~] B7 copiar menos na coleta de laço
  - [x] a tabela de salas cresce quando perde salas demais (50.000 elementos com push: de 1.160 para 210 ms)
  - [x] um registro das salas: uma coleta olha só as feitas depois da sua marca
  - [-] pré-promoção por lugar: não alcança o caso de `records`
  - [-] promoção no lugar: `records` 15% mais rápido, mas superlinear no Linux (memória guardada): desfeita
  - [-] reutilização no lugar (`xs = xs + [x]` crescendo onde está): mais lenta que um array novo pequeno
  - [-] barreira de escrita filtrada no código: as chamadas fazem trabalho real, sem ganho
  - [x] coletas completas que esperam conforme o lixo que acham; valores pequenos copiados sem a tabela: `records`
    de 1,95× para 1,6× o C
  - [ ] pré-promoção por chamada (o que `build()` faz vai para fora do laço): uma região guardada na arena
- [x] B8 o grafo sem JSON por dentro (os valores de `std.graph`; `collab.qk` sobre a árvore genérica, de propósito)
- [x] B9 concorrência
  - [x] `Channel<T>` tipado (o `T` dito onde o valor vai: `let c: Channel<Int> = channel()`)
  - [x] tudo o que espera, cooperativo (`connect`, nomes, `runProcess`, o teclado: numa thread própria)
  - [x] cancelamento e prazos (`cancel`, `withTimeout`, `receiveWithin`)
  - [x] concorrência estruturada (`scope`, `s.start`)
- [~] B10 compilação incremental de ponta a ponta
  - [x] `quark run` de um programa pequeno abaixo de 30 ms (38,5 → 20,6 ms)
  - [ ] rebuild do compilador abaixo de 0,5 s (hoje ~2,6 s): leitura, checagem e lowering guardados por módulo

## C. A linguagem

- [x] constantes em padrões, `T?!`, `a..=b` decidido (não), `Char` decidido (um `Int`), só `defer`, efeitos na doc,
  `edition` lida
- [x] o `?` convertendo erros (`E1` no caso do enum que o guarda; em texto num `T!`)
- [x] `Self` e implementações padrão em interfaces
- [x] `pub` por campo (num tipo opaco)

## D. Backends e sistema

- [x] escrever `Float` sem o C (dígitos mais curtos, exato)
- [x] ler `Float` sem o C (`strtod` não é mais usado)
- [x] ARM64 em paridade: argumentos em registradores (x0-x7; o código do compilador 8,8% menor), NEON já com as operações do x86-64
- [x] informação de depuração (`-g`): DWARF 4 nos objetos e CodeView no COFF (o PDB pelo linker); executáveis pela interface JIT do gdb
- [x] a suíte em wasm32: `quark build --target wasm32-wasi`, 124 testes verdes no wasmtime (CI), 12 que o wasm não tem
- [x] sem libc onde dá: o Windows só com kernel32 e ntdll; `{x:.2}` e `parseInt` sem C em todo sistema; o Linux fica
  com a libc (DNS, fusos e `dlopen` dependem dela)
- [x] libquark no macOS (Mach-O ligado, arquivo BSD), a API C revista (`qkVersion`, as bibliotecas do sistema, ARM64)
- [x] executáveis menores: só o runtime que o programa alcança (hello 911 → 136 KB)

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
