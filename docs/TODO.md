# A fazer

Onde está o [PLANO.md](PLANO.md): o que foi feito, o que foi tentado e deixado de fora (e por quê), o que falta. Os
detalhes, as medições e os motivos ficam no plano, sob cada item; esta lista só ajuda a achá-los.

Três partes: o que está **feito** (fases A a E do plano), o que **ficou para trás** (cada pendência dita como está)
e **para onde ir agora** (o próximo trabalho, em ordem).

`[x]` feito · `[~]` começado · `[ ]` a fazer · `[-]` medido ou pesado, e não feito (o plano diz por quê)

Estado em 21/11/2026: a versão publicada é a 0.9.2; o `main` tem as fases A a E (menos o que está abaixo, em "ficou
para trás"), com a CI verde em Windows, Linux x86-64 e ARM64, macOS (x86-64 e ARM64) e WebAssembly, o compilador
se reconstruindo nos mesmos bytes e a suíte inteira passando.

## Feito

### A. Rede de segurança

- [x] A1 git: [github.com/eugeniobenfica/quark](https://github.com/eugeniobenfica/quark), público, MIT
- [x] A2 release (0.9.2) e CI rodando a suíte inteira em Windows, Linux x86-64 e ARM64, macOS
- [x] A3 teste diferencial (`tools/difftest.qk`; a cada mudança, 3000 programas toda noite)
- [x] A4 fuzzing (`tools/fuzz.qk`), `quark bind` também (um caso em quatro: um cabeçalho C mudado ao acaso)
- [x] A5 as tabelas do runtime conferidas entre si
- [x] A7 forma canônica conferida na CI
- [x] `tools/parity.sh`: dois compiladores fazem o mesmo código (para refatorações)

### B. Estrutura

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
- [x] B7 copiar menos na coleta de laço (`records` 1,46× o C: a meta do 0.12)
  - [x] a tabela de salas cresce quando perde salas demais (50.000 elementos com push: de 1.160 para 210 ms)
  - [x] um registro das salas: uma coleta olha só as feitas depois da sua marca
  - [-] pré-promoção por lugar: não alcança o caso de `records`
  - [-] promoção no lugar: `records` 15% mais rápido, mas superlinear no Linux (memória guardada): desfeita
  - [-] reutilização no lugar (`xs = xs + [x]` crescendo onde está): mais lenta que um array novo pequeno
  - [-] barreira de escrita filtrada no código: as chamadas fazem trabalho real, sem ganho
  - [x] coletas completas que esperam conforme o lixo que acham; valores pequenos copiados sem a tabela: `records`
    de 1,95× para 1,6× o C
  - [x] a barreira de escrita lembra a última entrada; arrays pequenos copiados no lugar: `records` ~1,52× o C
  - [x] a coleta completa espera mais quando os dados só crescem; a checagem do turno numa leitura; `x % d == 0` sem
    arredondar; instruções afundadas no ramo que as usa: `records` 1,66× → 1,46× o C (o melhor de três), `json` −6%
  - [-] pré-promoção por chamada: não foi preciso para a meta (o desenho segue no plano)
- [x] B8 o grafo sem JSON por dentro (os valores de `std.graph`; `collab.qk` sobre a árvore genérica, de propósito)
- [x] B9 concorrência
  - [x] `Channel<T>` tipado (o `T` dito onde o valor vai: `let c: Channel<Int> = channel()`)
  - [x] tudo o que espera, cooperativo (`connect`, nomes, `runProcess`, o teclado: numa thread própria)
  - [x] cancelamento e prazos (`cancel`, `withTimeout`, `receiveWithin`)
  - [x] concorrência estruturada (`scope`, `s.start`)
- [~] B10 compilação incremental de ponta a ponta (o que falta está em "ficou para trás")
  - [x] `quark run` de um programa pequeno abaixo de 30 ms (38,5 → 20,6 ms)
  - [x] otimizar e o código de máquina nas threads, em partes (o compilador no tier rápido: 2,4 → 1,75 s)
  - [x] as constantes guardadas por conteúdo (`quark-consts-*.bin`): ler e checar 0,62 → 0,50 s, os mesmos bytes
  - [-] o lowering das funções em threads: feito e medido (800 → 650 ms, mas +700 MB de pico e +0,6 s de kernel: cada
    thread copia o programa); desfeito, o plano diz o que o faria valer
  - [ ] menos cópias da árvore e menos memória (3,5 GB no pico; 1,1 s de kernel em falhas de página)
  - [ ] o cache por módulo: leitura, checagem, lowering (o que leva de ~1,2 s a 0,5 s)

### C. A linguagem

- [x] constantes em padrões, `T?!`, `a..=b` decidido (não), `Char` decidido (um `Int`), só `defer`, efeitos na doc,
  `edition` lida
- [x] o `?` convertendo erros (`E1` no caso do enum que o guarda; em texto num `T!`)
- [x] `Self` e implementações padrão em interfaces
- [x] `pub` por campo (num tipo opaco)
- [x] `T` dentro de uma lambda de uma função genérica; `r.f(x)` chama o campo `f` mesmo com um valor `f` ao alcance
- [x] uma lambda guarda uma função dada à função genérica em volta (também dentro de um construtor genérico)

### D. Backends e sistema

- [x] escrever `Float` sem o C (dígitos mais curtos, exato)
- [x] ler `Float` sem o C (`strtod` não é mais usado)
- [x] ARM64 em paridade: argumentos em registradores (x0-x7; o código do compilador 8,8% menor), NEON já com as
  operações do x86-64
- [x] informação de depuração (`-g`): DWARF 4 nos objetos e CodeView no COFF (o PDB pelo linker); executáveis pela
  interface JIT do gdb
- [x] a suíte em wasm32: `quark build --target wasm32-wasi`, os testes verdes no wasmtime (CI), os que o wasm não
  tem listados com o motivo (`tests/wasm-skip.txt`)
- [x] sem libc onde dá: o Windows só com kernel32 e ntdll; `{x:.2}` e `parseInt` sem C em todo sistema; o Linux fica
  com a libc (DNS, fusos e `dlopen` dependem dela)
- [x] libquark no macOS (Mach-O ligado, arquivo BSD), a API C revista (`qkVersion`, as bibliotecas do sistema, ARM64)
- [x] executáveis menores: só o runtime que o programa alcança (hello 911 → 136 KB)

### E. Biblioteca e ferramentas

- [x] `std.csv`, `walkFiles`, `quark test --filter`, o tutorial
- [x] `std.encoding`, `std.hash`, `std.args`, `std.files`, `std.net`, `std.http`
- [x] TLS pela biblioteca do sistema (Schannel, OpenSSL carregada ao primeiro uso, SecureTransport) atrás de um motor
  de bytes por sistema no runtime: `tlsConnect`, `readTls`, `writeTls`, `closeTls`; `connectTls` em std.net,
  `https://` em std.http; um servidor por endereço IP conferido pelo endereço (a CI confere nos três sistemas)
- [x] HTTP: corpos em `chunked` lidos (cliente e servidor); uma resposta sem tamanho lida até o fim da conexão
- [x] JSON tipado: `encode(v)` e `decode(text)` por tipo (records, enums, arrays, mapas, opcionais, tuplas; padrões dos
  campos), feitos para cada tipo pelo checker quando usados
- [x] subprocessos com entrada e saída em fluxo: `startProcess`, `writeProcess`, `readProcess(Errors)`,
  `closeProcessInput`, `waitProcess`, `killProcess` (cooperativos em tarefas)
- [x] fusos horários com nome: `zone("Europe/Lisbon")` em std.time, pelos arquivos TZif do sistema (toda a
  história) ou pela regra de hoje de cada zona em std.zones (gerado do tzdata por `tools/zones.py`)
- [x] pacotes: versões semânticas (seleção mínima de versões, como a do Go: a mais nova das mínimas pedidas, uma
  major) e registro (um repositório git com `index.toml`; `registry =` ou `QUARK_REGISTRY`), `quark get` com hash
- [x] LSP: renomear e referências em todos os arquivos do programa, ir para implementações, tipos e efeitos ao passar
  o mouse, diagnósticos incrementais (edições por trecho, textos abertos vistos pelos outros arquivos, quem usa o
  arquivo verificado de novo, verificação guardada entre pedidos)
- [x] testes: `quark test --coverage` (as linhas que os testes rodaram), propriedades em std.testing (`forAll`,
  geradores, redução ao menor valor que falha); o arquivo certo num pânico em código de outro arquivo
- [x] guias: [vindo de Go, Rust, Python](COMING-FROM.md), [servidores](SERVERS.md) (seus exemplos verificados pela
  suíte); a [referência do prelúdio](PRELUDE.md) gerada por `quark doc` (seções do arquivo como títulos), conferida
  pela suíte

## Ficou para trás

O que as fases A a E pediam e não está feito. Cada item diz o que falta e por quê.

- [ ] **A6 assinar o `quark.exe` no Windows.** Depende de um certificado de assinatura de código (Azure Trusted
  Signing, OV ou EV), que só o dono do projeto pode tirar. Sem ele, o Smart App Control e o SmartScreen barram
  executáveis novos.
- [~] **B10 rebuild do compilador abaixo de 0,5 s** (meta do marco 0.12; hoje ~1,6 s: ler e checar 0,50 s com as
  constantes guardadas, lowering 0,80 s, otimizar 0,17 s, código de máquina 0,15 s). Começado em 21/11; os três
  passos que faltam, medidos e em ordem, estão acima em B10 e no [PLANO.md](PLANO.md).
- [ ] **SIMD fora do `--release`:** os laços só são vetorizados no tier otimizador.
- [-] **TLS do lado do servidor:** decidido que não por ora; um servidor Quark fala HTTP e o TLS fica num proxy
  na frente ([SERVERS.md](SERVERS.md)).

## Para onde ir agora

Em ordem. Os três primeiros fecham o marco 0.12 do plano; o resto leva ao 1.0 (a fase E feita e o congelamento:
[STABILITY.md](STABILITY.md) valendo, nenhuma mudança de sintaxe ou de layout pendente).

1. [ ] **Publicar uma versão nova (0.12).** A CI ainda começa da 0.9.2, então o compilador não pode usar o que veio
   depois dela. Uma versão nova destrava isso e põe TLS, JSON tipado, pacotes com versões e o LSP novo nas mãos de
   quem usa. Precisa do OK do dono para publicar.
2. [x] **Fechar as lacunas de E que um usuário encontra logo:** corpo `chunked` no HTTP, HTTPS por IP verificado na
   CI, lambdas que guardam funções.
3. [~] **Fechar o marco 0.12:** B7 feito (`records` 1,46× o C); falta B10 (rebuild abaixo de 0,5 s, com a checagem e o
   lowering guardados por módulo).
4. [ ] **Revisão para o congelamento.** Ler a std inteira como API pública: nomes, ordem dos parâmetros, o que é
   `pub`; tudo o que ficaria errado depois do 1.0 decidido agora. O mesmo para a sintaxe (cada forma canônica
   revista uma última vez) e para o layout dos valores que a API C expõe.
5. [ ] **As ferramentas onde as pessoas estão:**
   - uma extensão de editor (VS Code) que leve o `quark lsp` e o `quark dap`;
   - um registro de pacotes público (o repositório `index.toml`);
   - um playground no navegador (o compilador em wasm32);
   - instaladores por sistema.

   Cada um é público: precisa do OK do dono.
6. [ ] **Desempenho, de novo medido:** atualizar a tabela abaixo em todos os benchmarks, nos três sistemas, e
   escolher o próximo alvo pelo que estiver mais longe do C.
7. [ ] **1.0:** STABILITY.md revisto, o changelog desde a 0.9, o anúncio.

## Desempenho contra C (gcc -O2)

Medido em 21/11/2026 (`bash tools/bench.sh`, o melhor de três). Mais rápido que C: pipeline (0,42×), strings (0,59×),
sorting (0,76×), trees (0,20×). Empatados ou até 7%: words (0,99×), mandelbrot (1,03×), counting (1,07×). Atrás:
records (1,46×: o coletor de laço copia o que sobrevive).
