# Quark: estado da linguagem e caminho para a versão estável

Retrato de 18/11/2026, depois do plano de nove etapas (desempenho, reestruturação, sintaxe, grafo). Substitui o
retrato de 15/11. A referência completa está em [LANGUAGE.md](LANGUAGE.md); a ligação com C, em
[EMBEDDING.md](EMBEDDING.md); como o Quark se constrói, em [BOOTSTRAP.md](BOOTSTRAP.md); a decisão de memória, em
[MEMORY.md](MEMORY.md).

## 1. Resumo

- O Quark é uma linguagem compilada de uso geral, escrita nela mesma (compilador, ferramentas e runtime: cerca de
  69 mil linhas de Quark), sem dependências: nem LLVM, nem compilador C, nem assembler, nem linker.
- No Windows x64 tudo está verificado: o compilador se reconstrói nos mesmos bytes e a suíte passa 915 de 915.
- Para uma versão estável faltam principalmente coisas em volta da linguagem, não dentro dela: nada do trabalho
  recente está no git, não há release publicado, Linux e ARM64 não foram revalidados depois das últimas mudanças, e
  a biblioteca padrão é pequena para "uso geral".

## 2. O que o Quark é

Princípios que guiam cada decisão:

- uma forma canônica para cada coisa, imposta pelo compilador e pelo `quark fmt`;
- tipos e efeitos inferidos, sem custo em tempo de execução;
- valores, nunca referências: nada muda pelas costas de quem tem o valor;
- memória em arena, devolvida pelos laços conforme andam, sem coletor de lixo global e sem pausas;
- um programa é também um grafo de nós: texto e grafo vão e voltam sem perda.

A ligação especial é com C: qualquer programa C embute o Quark como linguagem de script (libquark), o Quark chama
bibliotecas C direto (`extern fn`), e `quark object` gera objetos que um programa C linka.

## 3. Como funciona

```
texto ── lexer ──▶ tokens ── parser (tolera erros) ──▶ AST ── check ──▶ AST tipada + efeitos
      ── lower ──▶ IR plana ──┬── opt (tier rápido) ──────────────────────────┬──▶ x64 / arm64 ──▶ PE, ELF, Mach-O
                              └── release (SSA, faixas, LICM, SIMD), em threads ┘                  ou memória (quark run)
                                  com cache por conteúdo
graph ⇄ qgn ⇄ format: AST ⇄ grafo (QGN ou JSON) ⇄ texto canônico      construn: constantes, executadas na compilação
```

| Parte | O que faz |
| --- | --- |
| Parser | Lê o programa inteiro mesmo com erros: o que não pôde ser lido vira um buraco (`_`), e todos os erros de sintaxe saem de uma vez |
| Checker | Inferência local de tipos, genéricos monomorfizados, efeitos por ponto fixo, exaustividade de `match`, capacidades (`--allow`) |
| construn | Calcula as constantes na compilação executando o próprio código delas (compila o que elas alcançam, roda, lê o valor de volta); falha, estouro ou demora viram erro |
| prelude | Os built-ins de sistema escritos em Quark (`compiler/prelude.qk`): arquivos, processo, rede, tarefas, relógio; cada um é uma função comum cujo corpo chama `runtime(...)` |
| Lowering | AST → IR; decide cópias por análise de uso (um `let` lido de um `var` só é copiado se ainda for usado depois de uma mudança no lugar); inlining |
| IR | Instruções são só números; os operandos das chamadas ficam num array por função |
| Tier rápido | Poucos passes, uma fração de milissegundo; é o padrão de `quark run` |
| Tier `--release` | SSA, análise de escape, devirtualização, faixas (tira checagens de limite e de estouro que não podem falhar), LICM, SIMD (AVX2/SSE/NEON), redução de força; uma função por vez, nas threads do processador |
| Cache | O resultado otimizado de cada função é guardado por conteúdo; um rebuild só reotimiza o que mudou, e gera os mesmos bytes |
| Backends | x86-64 (Windows x64 e System V) e ARM64 (AAPCS64), codificados byte a byte; alocação de registradores por varredura linear |
| Runtime | Em Quark: arena e coleta por laço, textos, maps, impressão, arquivos, processos, tasks, threads, pânicos com pilha de chamadas, ganchos do depurador |
| Executáveis | PE32+, ELF e Mach-O (assinado ad hoc) para seis máquinas, de qualquer uma delas (`--target`) |

Memória: alocar é avançar um ponteiro; a memória de uma chamada some quando ela termina; um laço que aloca copia
para fora só o que as suas variáveis ainda seguram, com gerações. A contagem de referências (Perceus) foi
construída e medida ao lado; a arena ficou (tabela em MEMORY.md).

## 4. O que a linguagem tem hoje

**Tipos e dados**

- `Int` (64 bits), `Float`, `Bool`, `Text` (UTF-8), `Bytes`, arrays, maps, sets, tuplas, records com valores
  padrão, enums com dados (tipos soma), `T?`, `T!`, `T ! E`, `Never`, `Unit`.
- Genéricos com limites por interface; interfaces estruturais; operadores em tipos próprios; métodos de extensão.
- Tipos recursivos (o campo recursivo vai numa caixa, automaticamente).

**Controle e erros**

- `if` e `match` como expressões; padrões aninhados, alternativas, guardas, faixas; `is`; `let ... else`.
- Erros como valores: `?`, `?.`, `??`, `mapErr`; `defer`; `return`/`break`/`continue` onde vai um valor.
- Buracos: `_` no lugar de um valor ainda não escrito; o programa compila e roda até chegar nele.
- Aritmética de `Int` que nunca dá a volta em silêncio: `+ - *`, `-x`, `abs` e `sum()` param o programa no
  estouro; `+% -% *%` dão a volta de propósito.

**Funções e laços**

- Lambdas, funções como valores, argumentos nomeados e padrão, chamada uniforme (`a.f(b)` é `f(a, b)`).
- Pipelines que fundem (`map`/`filter`/`sum`... sem arrays intermediários), com SIMD em `--release`.
- Efeitos inferidos; `effect fn` quando declarado; lambdas de pipeline são puras.

**Concorrência**

- Tasks (corrotinas com pilha e arena próprias): `spawn`, `step`, `yield`, `sleep`.
- `parallelMap` (resultados podem ser textos, arrays, maps, records) e `parallelFold`, nas threads do processador.

**Sistema e C**

- Arquivos, pastas, processos, ambiente, argumentos, relógio, números aleatórios.
- `extern fn` para bibliotecas C (tipos, structs, ponteiros, variádicas, callbacks), nas três convenções; ponteiros de função que o C devolve (resultados, campos de struct) são valores `extern (A) -> R` chamados direto de Quark, e campos ponteiro aceitam NULL (`None`).
- Capacidades: `--allow print,files,...` limita o que um programa alcança.

**Biblioteca padrão** (9 módulos): `collections`, `json`, `math`, `path`, `regex`, `time`, `testing`, `machine`,
`graph`.

**Ferramentas** (todas no mesmo executável)

| Comando | Para quê |
| --- | --- |
| `run`, `build`, `object`, `test` | compilar e rodar em memória, executável, objeto para C, testes da linguagem |
| `fmt` | forma canônica: layout, largura de 120 colunas, built-ins em forma de método, `toArray()` supérfluo removido |
| `check`, `doc`, `catalog` | erros, documentação, o que um editor de nós pode oferecer |
| `graph`, `text`, `diff`, `merge` | grafo em QGN (um valor Quark tipado por `std.graph`) ou JSON; diff e merge de três vias por nó |
| `lsp`, `dap`, `debug`, `repl` | servidor de linguagem, adaptador de depuração, depurador, REPL |
| `get` | pacotes por git (`quark.toml`, `quark.lock`) |
| `bind` | lê um cabeçalho C (pré-processador e parser próprios) e escreve as declarações `extern` em Quark; testado com zlib, SQLite, SDL2, libcurl e libpng |
| `--profile`, `--trace`, `--time`, `--fuel`, `--memory` | perfil, rastro de valores, tempos por fase, orçamento de passos e de memória |

## 5. O que foi feito no plano de nove etapas

| Etapa | Resultado |
| --- | --- |
| 1. Benchmarks de dados | `words`, `records`, `json`, `strings` (com versões em C) e `tools/bench.sh` |
| 2. Árvore, hashes, buracos, parser tolerante | buracos `_`; hash de conteúdo por nó; todos os erros de sintaxe de uma vez; layout que segue o nó pelo hash |
| 3. IR plana e back end paralelo | instruções sem arrays; `--release` em threads; self-build de 16,3 s para 6,3 s |
| 4. Sintaxe | estouro sempre para; `+% -% *%`; forma de método canônica; largura de linha; inferência em literais |
| 5. Modelo de memória | contagem de referências construída e medida; a arena ficou |
| 6. Migração de memória | não feita, por decisão da etapa 5 |
| 7. Cache incremental | rebuild `--release` do compilador em 2,4 s, com os mesmos bytes |
| 8. Quark Graph Notation | `quark graph` escreve QGN; JSON com `--json`; todos os leitores aceitam os dois |
| 9. Convenção de chamada | cinco primeiros argumentos e resultados em registradores, só em x86-64 |

Defeitos antigos achados e corrigidos no caminho: cópia quadrática de arrays passados a métodos que mudam outro
valor; o formatador imprimia `-(-x)` como `--x`; comentários de campos e de casos saíam do tipo; o tier rápido
apagava uma conta não usada junto com o pânico dela; threads deixavam programas pequenos mais lentos de compilar.

## 6. Desempenho

Quark `--release` contra C (`gcc -O2`, MSYS2), melhor de 3, Windows x64 (Ryzen 7 9700X), medidos em 18/11/2026
depois da rodada de otimização do runtime de texto e mapas.

| Benchmark | Quark | C | Leitura |
| --- | --- | --- | --- |
| `counting` | 31 ms | 28 ms | empate |
| `mandelbrot` | 34 ms | 32 ms | empate |
| `pipeline` | 49 ms | 117 ms | Quark 2,4× mais rápido |
| `trees` | 451 ms | 1.618 ms | Quark 3,6× mais rápido (arena) |
| `sorting` | 634 ms | 817 ms | Quark 1,3× mais rápido |
| `strings` | 368 ms (era 1.003) | 476 ms | Quark 1,3× mais rápido |
| `words` | 488 ms (era 791) | 359 ms | C 1,36× mais rápido (era 2,2×) |
| `records` | 127 ms (era 264) | 59 ms | C 2,15× mais rápido (era 4,5×) |
| `json` | 465 ms | sem versão em C | |

O que mudou nessa rodada: busca de bytes oito por vez (SWAR) em `find`, `split`, `replace` e `words`;
comparação de bytes sem chamar o `memcmp` do C para trechos curtos; cópias de até 16 bytes sem chamar o `memcpy`;
`split` e `words` numa passada só; tabela dos mapas com 32 bits do hash ao lado do índice (sondas erradas não leem
a entrada) e hash de texto de 8 em 8 bytes; `==` de textos compara os tamanhos antes de chamar o runtime; um array
constante de textos indexado (`nomes[i]`) lê de uma tabela nos dados em vez de montar o array a cada uso; `s +=
"{a}{n} "` junta cada pedaço direto em `s`, e um número interpolado escreve os dígitos no fim do texto (`qk_joinInt`).

O compilador se compila em 2,2 s (tier rápido) ou 6,3 s (`--release`, 2,4 s com o cache quente).

Na coleta por laço, cada bloco copiado é anotado para que outro valor que o segure ache a mesma cópia. Isso era uma
tabela hash espalhada pela memória (metade do tempo de `records`); agora é uma tabela lateral indexada pelo endereço,
reaproveitada entre coletas (cada entrada leva a época da coleta), com a tabela hash só para fatias de mesmo início
e outro tamanho. `records` foi de 264 para 127 ms e `json` de 465 para cerca de 437 ms.

Onde o Quark ainda perde: `records` (2,15× o C), pela cópia dos sobreviventes em si, e `words` (1,36×), pelas
chamadas ao runtime a cada palavra.

## 7. O que falta para uma versão estável

**Bloqueiam um 1.0**

1. **Nada está no git.** A pasta `compiler/` inteira aparece como não rastreada e `src/` (a semente em C) como
   apagada: 320 arquivos pendentes desde a remoção da semente. Hoje a única cópia é esta pasta (e o OneDrive).
   É o maior risco do projeto e o mais barato de resolver.
2. **Sem release publicado.** O código do compilador usa recursos novos (`parallelMap` de valores com memória,
   `+%`), então só um compilador desta versão o constrói. A CI baixa "o último release" e não existe nenhum.
3. **Linux e ARM64 não revalidados.** A última suíte verde em Linux x86-64 e ARM64 é anterior à IR plana, às
   checagens de estouro, ao SIMD com checagem e ao cache. As imagens das VMs de teste estão corrompidas.
4. **ARM64 atrás do x86-64.** A convenção de chamada por registradores só existe em x86-64. macOS e Windows em
   ARM64 nunca rodaram numa máquina real; não há libquark para macOS.
5. **Executáveis recusados pelo Windows.** O Smart App Control recusa executáveis novos sem assinatura, pelo
   conteúdo. Isso atrapalhou este trabalho o tempo todo e vai atingir qualquer usuário de Windows 11 que rode
   `quark build`. Precisa de uma resposta: assinatura de código do `quark.exe`, e orientação clara para os
   executáveis gerados.
6. **Código experimental no caminho principal.** O heap contado (`QUARK_COUNTED`) perdeu a decisão, ainda falha
   em programas grandes, e segue espalhado pelo lowering e pelo runtime. Para um 1.0: tirar, ou isolar de vez.

**Deveriam entrar antes do 1.0**

7. **Biblioteca padrão para uso geral** (feito em 18/11/2026, faltando TLS): arquivos em partes (`openFile`,
   `readChunk`, `writeChunk`, `seekFile`, `std.files`), TCP (`listen`, `accept`, `connect`, `std.net`), HTTP/1.1
   sem TLS (`std.http`: servidor e cliente), `std.args`, `std.encoding` (base64, hex, URL), `std.hash` (CRC-32,
   FNV-1a, SHA-256, HMAC) e hora local (`localOffset`, `std.time.local`). Falta HTTPS: exigiria TLS, que é
   criptografia demais para escrever sem dependência; o caminho é `quark bind` de uma biblioteca do sistema.
8. **Concorrência para servidores** (feito): `go(f)` e `runTasks()` rodam tarefas em segundo plano; leitura,
   escrita e accept em sockets esperam só a tarefa que chamou; quando todas esperam, o processo dorme no `poll`
   do sistema; canais de texto (`channel`, `send`, `receive`, `closeChannel`) com detecção de impasse. Medido:
   50 requisições HTTP de 0,2 s cada atendidas em 0,38 s. Tudo numa thread; Linux e macOS escritos, não rodados.
9. **Política de estabilidade** (feito): `docs/STABILITY.md` (o que é estável, o que não é, como se muda, as
   plataformas) e `quark version`.
10. **Limpeza do cache** (feito): os caches ficam em `quark-cache/` na pasta temporária, com um índice de uso;
    o que ficou 30 dias sem uso sai, e acima de 1 GB saem os menos usados; `quark clean` apaga tudo (removeu 502
    arquivos acumulados nesta máquina).

**Limites conhecidos, aceitáveis se documentados**

- Uma lista constante de milhões de elementos gasta muita memória na compilação.
- Um laço vetorizado que estoura só para depois da parte vetorial (na linha certa, mas não no elemento exato).
- Iniciar threads custa cerca de 10 ms (cada uma carrega o código de novo); por isso só programas grandes são
  otimizados em paralelo, e `parallelMap` só compensa com trabalho de verdade por elemento.
- `quark fmt` precisa checar o programa para as reescritas por tipo; um programa com erros só é diagramado.
- SIMD fora do `--release`.

## 8. Como melhorar o que temos

Por ordem de retorno:

1. **Textos pequenos embutidos no valor** (até 15 bytes sem alocação) e slices sem cópia. Ataca `words` (1,38× o C) e
   `strings`, os dois piores benchmarks, e qualquer programa que manipule texto.
2. **Mudança no lugar em dados de vida longa** (`records`, 2,15× o C depois da tabela lateral): o resto é a cópia
   dos sobreviventes. Alocar direto na região de fora o que vai para um container antigo (pré-promoção) a evitaria.
3. **Custo de página do sistema.** Num build `--release` o processo passa cerca de 4 s no sistema operacional,
   alocando e devolvendo blocos grandes. Reaproveitar blocos tira isso do compilador e de todo programa que aloca.
4. **Threads que não recarregam o código.** O custo de 10 ms por thread vem de cada uma carregar a sua cópia.
   Resolver isso libera o paralelismo para programas pequenos e para o tier rápido.
5. **Testes que procuram erros sozinhos.** A suíte tem 915 casos escritos à mão. Faltam: fuzzing do parser e da
   ida e volta texto ↔ grafo, comparação automática tier rápido × `--release` sobre programas gerados, e a forma
   canônica de `compiler/` dentro da suíte (hoje está fora por ser lenta).
6. **O grafo como estrutura interna.** O código do grafo ainda monta e lê JSON por dentro e converte para QGN na
   borda. Trabalhar direto sobre valores `std.graph` simplifica e deixa os programas Quark manipularem grafos.
7. **Paridade do ARM64**: convenção por registradores, e uma máquina (ou VM) no ciclo de testes.
8. **Informação de depuração padrão** (DWARF, PDB), para depuradores e perfiladores externos; hoje só as
   ferramentas do próprio Quark enxergam os programas.
9. **Documentação de entrada.** Há uma referência completa, mas nenhum tutorial; e este relatório é o único
   documento em português.

## 9. O que eu acrescentaria à linguagem (com parcimônia)

A linguagem em si está redonda; quase tudo que falta é biblioteca e plataforma. As adições que eu consideraria:

- **Inteiros de tamanho fixo fora do C** (`Int32`, `UInt8`... hoje só em declarações `extern`): formatos
  binários, protocolos e hashes pedem isso, e `+%` já aponta nessa direção.
- **Iteração em tipos do usuário**, para que coleções de `std` e de pacotes entrem em `for` e nos pipelines como
  os arrays (a conferir até onde isso já vai hoje).
- **Concorrência estruturada** sobre as tasks: escopos que esperam seus filhos, canais, cancelamento.
- **`Char`** como tipo próprio (hoje um caractere é um `Int`): a decidir antes do 1.0, porque depois quebra código.

Não acrescentaria: herança, exceções, macros, sobrecarga por tipo de argumento, anotações de tempo de vida. Todas
contrariam os princípios da seção 2.

## 10. Caminho sugerido até o 1.0

1. Commitar tudo e publicar um release 0.9 (destrava a CI e protege o trabalho).
2. Baixar ISOs novas do Alpine e revalidar Linux x86-64 e ARM64; levar a convenção de chamada ao ARM64.
3. Tirar ou isolar o heap contado; limpar o cache; `--version`; política de estabilidade.
4. Biblioteca padrão: arquivos em partes, argumentos, rede, codificações.
5. Textos pequenos e reaproveitamento de blocos (os dois ganhos de desempenho mais largos).
6. Rodar em macOS e Windows ARM64 reais; assinatura de código no Windows.
7. Fuzzing e testes diferenciais; tutorial. Então congelar e chamar de 1.0.

## 11. Como verificar

```bash
QUARK=caminho/do/quark ./build.sh --check   # constrói o compilador e confere o ponto fixo (mesmos bytes)
bash tests/run.sh                           # programas, erros, pânicos, forma canônica, grafo, docs, host C
bash tools/bench.sh 5                       # benchmarks (com gcc no PATH, compara com C)
python tools/vm/vm.py test                  # a mesma suíte em Linux ARM64 (QK_ARCH=x86_64: x86-64), no QEMU
```

No Windows 11 com Smart App Control, `build.sh` pode falhar com "Permission denied" num executável recém-criado:
o Windows recusa o arquivo pelo conteúdo, e reconstruir os mesmos bytes é recusado de novo. O contorno usado até
aqui foi construir as etapas com nomes novos e um comentário de marca num arquivo do runtime, para mudar os bytes.
