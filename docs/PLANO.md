# Plano de lapidação e refinamento do Quark

Escrito em 18/11/2026, depois das rodadas de C (bind, ponteiros de função), desempenho do runtime, biblioteca
padrão e servidores. Complementa o [RELATORIO.md](RELATORIO.md) (o que existe) e o [STABILITY.md](STABILITY.md) (o
que será prometido). Aqui está o que mudar antes de prometer.

A ideia central: **o 1.0 congela contratos; tudo o que for estrutural tem de acontecer antes dele.** Por isso o
plano põe as grandes refatorações cedo e deixa para depois o que pode evoluir sem quebrar ninguém.

## 0. Diagnóstico: onde está o peso

| Medida | Valor | O que diz |
| --- | --- | --- |
| `compiler/lower.qk` | 9.341 linhas | um arquivo faz açúcar, decisões de memória, C, built-ins e emissão de IR |
| `compiler/check.qk` | 8.569 linhas | tipos, inferência, efeitos, built-ins e templates juntos |
| `compiler/consteval.qk` | 5.001 linhas | um **segundo interpretador** da linguagem, só para constantes |
| ramos `if b == bAlgo` | 150 no checker, 80 no lowering, 218 no consteval | cada built-in é escrito três vezes, à mão |
| lugares para ligar um built-in | 7 | esquecer um corrompeu o heap nesta sessão |
| menções a `counted` no lowering | 80 | um experimento que perdeu a decisão ainda mora no caminho principal |
| tempo no runtime (words, records) | 50 a 90% | o código gerado já empata com C; o custo está nas chamadas |
| plataformas rodadas a cada mudança | 1 de 6 | Linux, macOS e ARM64 têm código escrito e não executado |

Três causas explicam quase tudo: **semântica duplicada** (built-ins e constantes), **camadas que faltam** (do
programa checado direto para a IR de máquina) e **fronteira opaca com o runtime** (toda operação é uma chamada).

## 1. Princípios

1. Cada regra da linguagem escrita num lugar só. O que hoje existe em três (checker, lowering, consteval) passa a
   ser derivado de um.
2. Toda mudança estrutural entra atrás de uma comparação: a saída nova contra a antiga em toda a suíte, e o ponto
   fixo do compilador. Nenhuma "grande virada" sem as duas versões lado a lado por algum tempo.
3. Medir antes e depois (`tools/bench.sh`, tempo de self-build, tamanho do executável). Otimização que não aparece
   na medida sai, como saiu o runtime no tier otimizador.
4. O que não ganhou a decisão é removido, não mantido "por via das dúvidas".
5. Sem dependências externas. O que exigir uma (TLS) entra por `quark bind` de biblioteca do sistema, opcional.

## 2. Fase A: rede de segurança (antes de qualquer refatoração)

Sem isto as fases seguintes são arriscadas demais. Tudo aqui é pequeno.

| Item | O que é | Pronto quando |
| --- | --- | --- |
| A1. Git | commits organizados de `compiler/`, remoção de `src/`, tags | `git status` limpo; um clone novo compila |
| A2. Release 0.9 | os 8 alvos de `tools/release.sh`, publicados | a CI baixa esse release e roda a suíte em Linux e macOS |
| A3. Teste diferencial | gerador de programas; compara tier rápido, `--release`, `--no-avx` e `--abi sysv` | roda na CI; já achou ou descartou a instabilidade do runtime otimizado |
| A4. Fuzzing | parser, `fmt` (ida e volta), texto ↔ QGN, `quark bind` | nenhuma entrada derruba o compilador |
| A5. Checagem de consistência | o build falha se o número de funções do runtime, de nomes e de built-ins não bate | o erro do `runtimeCount` não pode mais acontecer |
| A6. Assinatura no Windows | `quark.exe` assinado; orientação para executáveis gerados | um Windows 11 com Smart App Control roda o release |
| A7. Forma canônica na suíte | `compiler/` e `std/` conferidos com cache por conteúdo | não é mais passo manual |

> **Estado em 19/11/2026.**
> - **A1. Feito.** O repositório é [github.com/eugeniobenfica/quark](https://github.com/eugeniobenfica/quark), público, com licença MIT. Os commits estão por área e há tags `v0.9.0`, `v0.9.1` e `v0.9.2`.
> - **A2. Feito.** O release 0.9.2 traz os 8 compiladores e o `SHA256SUMS`. Cada workflow baixa o último release e vai até o ponto fixo. Depois roda a suíte inteira:
>   - Windows x86-64, Linux x86-64 e Linux ARM64: verdes;
>   - macOS: Apple silicon e Intel, nas imagens `macos-15`.
>
>   A primeira rodada em Linux x86-64 achou um erro real do System V, corrigido no 0.9.1 e no 0.9.2. C pode mudar todos os registradores xmm, e as chamadas do próprio Quark preservam xmm8-15. Mas só os valores da função que chamava C eram salvos, não os de quem a chamou. O compilador 0.9.0 para Linux falhava no próprio build.
> - **A3. Feito.** `tools/difftest.qk` gera programas e compara o tier rápido com `--release`, `--no-avx` e `--abi sysv`. Roda na CI a cada mudança (300 programas) e toda noite (3000).
> - **A4. Feito.** `tools/fuzz.qk` muta os programas de `tests/run` e confere três coisas: `check`, a estabilidade do `fmt` e a ida e volta texto ↔ QGN. Roda na CI. `quark bind` ainda não entra.
> - **A5. Feito.** A suíte confere `runtimeCount` contra `runtimeNames`.
> - **A6. Não dá para fazer daqui.** Assinar exige um certificado de assinatura de código, e o Smart App Control só confia em executáveis assinados ou de boa reputação. O caminho é o Azure Trusted Signing, ou um certificado OV/EV, assinando os `.exe` do release na CI. Esse passo depende do dono do projeto. Para os executáveis gerados, a orientação é:
>   - `quark run` roda na memória, sem criar executável;
>   - quem distribui assina o seu executável.
> - **A7. Feito.** O workflow Checks confere a forma canônica de `compiler/`, `compiler/runtime/` e `tools/`.

## 3. Fase B: refatorações estruturais

São as propostas grandes. Cada uma diz por que vale o custo.

### B1. Built-ins declarados em Quark (um prelúdio), não em três arquivos

> **Feito em 19/11/2026 para os built-ins de sistema** (42 funções: arquivos, processo, ambiente, rede, tarefas,
> canais, relógio, acaso). Estão em `compiler/prelude.qk` como funções comuns, cujo corpo chama `runtime(group,
> name, values...)`; tipo, capacidade e documentação vêm da declaração. Os built-ins de coleções, texto e
> matemática continuam especiais: são os que o lowering e o otimizador especializam (laços fundidos,
> vetorização), e passá-los ao prelúdio depende de B5 (runtime inlinável) para não perder desempenho.

**Hoje:** cada built-in tem tipo no checker, código no lowering, e outra implementação no consteval.
**Proposta:** um arquivo `compiler/prelude.qk` declara todos como funções Quark comuns. A maioria ganha corpo em
Quark (`fn words(t: Text) -> [Text] = intrinsic.split(t, "")`); só um núcleo pequeno (uns 30 intrínsecos:
alocar, ler e escrever memória, chamar o sistema) continua especial, numa tabela única da qual saem ids, nomes do
runtime, catálogo e documentação.
**Por quê:** elimina os 7 lugares e os ~450 ramos; a std e os built-ins viram a mesma coisa; mensagens de erro,
argumentos nomeados e `fmt` passam a valer igual para todos; adicionar um built-in vira escrever uma função.
**Risco:** tempo de compilação (o prelúdio é checado sempre) — resolve-se com o cache de B10. Mensagens
especializadas de hoje precisam ser preservadas com anotações.
**Tamanho:** grande (semanas). Faz-se built-in por built-in, com a suíte verde a cada passo.

### B2. Constantes por execução, não por um segundo interpretador

> **Feito em 19/11/2026.** `compiler/consteval.qk` (5.001 linhas) saiu. Uma constante é compilada (só o que as
> constantes alcançam; o resto vira um buraco que para se for chamado) e executada com orçamento de passos e de
> memória; o valor volta por um despejo tipado (`compiler/construn.qk`). Antes da troca, as duas formas foram
> comparadas lado a lado: 644 de 644 constantes do compilador e 154 de 154 dos testes e exemplos deram o mesmo
> valor. Custo: cerca de 150 ms no build do compilador. Onde nada executa código (LSP, `qkVm`), a constante é
> calculada no lugar onde é usada.

**Hoje:** `consteval.qk` (5.001 linhas) reinterpreta a linguagem para avaliar `const`.
**Proposta:** o compilador já compila e roda código em memória. Uma constante é compilada como função e executada
em tempo de compilação, com orçamento de passos e de memória e sem efeitos; o valor resultante é gravado nos dados.
**Por quê:** some o maior foco de divergência (o que a constante calcula contra o que o programa calcula); toda
função pura passa a valer em constantes de graça; 5.000 linhas a menos.
**Risco:** compilação cruzada (o host executa, o alvo recebe: os valores têm o mesmo layout em todos os alvos de
64 bits, mas `wasm32` exige serialização cuidadosa) e o bootstrap (o compilador precisa de si mesmo para avaliar
as próprias constantes; o tier rápido resolve). Manter o interpretador só para o caso `wasm32` hospedado, se
preciso, numa versão mínima.
**Tamanho:** grande. Depende de B1 (built-ins executáveis) para render tudo.

### B3. Uma IR intermediária entre o checker e a IR de máquina

> **Começado em 19/11/2026 pelo passo 2.** O passo 1 (desaçucarar) foi medido antes e deixado de lado. `is`,
> `let ... else`, `with` e a interpolação somam umas 200 linhas das 8.483 do lowering. O grosso está nos built-ins
> especializados e nas decisões de memória.
>
> **Feito:**
> - As decisões de memória que dependem só da árvore checada saíram de `lower.qk` para `compiler/ownership.qk`:
>   - cópia, cópia funda, empréstimo ou tomada de um `let`;
>   - aliasing fundo;
>   - mudanças das funções `var self`;
>   - o que um laço muda e faz.
>
>   O tipo `Ownership` não lê nem escreve código.
> - O aliasing fundo deixou de ser escrito pela emissão. Antes, cada padrão, `for` e `let` gravava `aliasDeep` enquanto o código era gerado. Agora uma passada própria (`decideAliases`) decide tudo antes, na ordem em que o código é feito, numa tabela por binding e outra por `let` (`borrowed`).
> - Os acessores da árvore (`aOf`, `argsOf`, `writtenArgs`...) passaram a ser do `Ast`.
>
> **Números:**
> - `lower.qk` foi de 8.483 para cerca de 7.450 linhas.
> - O critério é `tools/parity.sh`. O IR de 209 programas, nos dois tiers, sai igual byte a byte com o compilador antigo e o novo, e o código de máquina do próprio compilador também.
> - Custo: a passada separada percorre a árvore mais uma vez. São cerca de 5% do tempo de lowering, uns 2% de compilar o compilador.
>
> **A primeira análise nova: último uso (`decideTakes`).** Um `var` de array ou mapa entrega o seu valor no último uso, em vez de copiá-lo:
> - empurrado numa lista;
> - guardado num campo;
> - devolvido.
>
> O último uso é a única menção do var no último comando do seu bloco que o menciona. Ela não pode estar sob um laço ou lambda, nem pode haver `defer`, cópia de lambda ou `let` preguiçoso que o leia depois. Com isso some uma cópia inteira por linha em padrões como `rows.push(row)`.
>
> Resultados:
> - O código mudou em 17 dos 209 programas da paridade: json, csv, regex, stdlib e o próprio compilador.
> - A suíte, o ponto fixo, o difftest (600 programas) e o fuzz passam.
> - Ganho medido de 2–3% em `bench/json.qk` e num laço de linhas.
> - Custo de compilação de uns 5 ms.
>
> A tabela de funções do runtime que não alocam (para a coleta por turno de laço) era uma lista de números. Virou nomes em `ir.qk` (`runtimeMakesNothing`), e um nome que o runtime não tem impede o build.
>
> **Guiado pelos benchmarks contra C (gcc -O2), em 20/11/2026:**
> - **Records.** Um record construído onde o valor precisa ser possuído por inteiro (`self.code[k] = Ins(...)` sobre um var fundo) era montado com cópias rasas e depois copiado fundo por inteiro. Agora cada campo é possuído por si (`Lower.construct(e, deep)`). Uma junção de arrays de valores simples (`args + [x]`) conta como nova (`deepFresh`). No laço quente, as cópias por instrução foram de 3 para 1: records de 123 para 115 ms.
>   - O que resta contra C (2,1×) é o coletor copiar os arrays novos que sobrevivem por irem para uma estrutura velha. Isso é próprio do desenho geracional por cópia.
> - **Words.** `len(c)` de um array constante montava o array inteiro a cada volta do laço interno, só para ler o tamanho. Agora é uma constante: words de 1,51× para 1,35× de C.
> - **As palavras de um texto, sem array.** `text.words()` é entregue palavra por palavra a quem a percorre: `for`, `countBy`, `map`, `filter` e as outras fusões de laço. Um laço no IR acha cada palavra, e o byte comum, acima de `' '`, custa um só teste.
>   - Resultados: um `for` sobre as palavras foi de 85 para 22 ms; `text.words().countBy(...)` de 248 para 163 ms; words fica em 1,08× de C.
>   - A primeira versão tinha um erro. Num `for`, o texto era feito depois da marca do laço: uma coleta no corpo o tomava de volta enquanto ainda era lido, e o array antigo continuava sendo feito à toa. Agora o texto é feito antes da marca (`preLowerSource`).
>   - No mesmo lugar havia um erro antigo: `take` e `skip` não eram atravessados até a fonte, e `for x in f().take(n)` lia memória já coletada. Os dois estão cobertos por `tests/run/loopSourceMade`.
>   - `split` e `lines` foram medidos fundidos do mesmo jeito e ficaram mais lentos: `lines` 22 → 37 ms, `split` 102 → 111 ms. O runtime acha o separador oito bytes por vez, e um laço byte a byte no IR perde. Ficam como estão, e `tests/run/textPieces` fixa o que eles dão.
> - O `pipeline` deixou de acusar diferença no checksum. Era o mesmo Float escrito de dois jeitos, e `tools/bench.sh` agora compara os números por valor.

> **Mais uma rodada (20/11/2026):**
> - **Arrays constantes de texto, percorridos sem array.** `for`, `map`, `countBy` etc. leem a tabela nos dados, como o índice já fazia. Num laço de teste, 46 → 36 ms e nenhuma alocação.
> - **Resultado já possuído (`decideResults`).** Uma função cujo valor devolvido é sempre todo seu não paga cópia funda onde a chamada é guardada. Conta como todo seu:
>   - algo novo;
>   - um var fundo seu;
>   - a chamada de outra função assim;
>   - um `if`, `match` ou bloco em que todos os ramos são assim.
>
>   O cálculo é um ponto fixo otimista. Os `return` de cada função são anotados pela passada de aliasing, que já percorre tudo, então o custo é perto de zero.
> - **Tempo de compilação.** O compilador compilando a si mesmo ficou cerca de 5% mais rápido no total, já pagando as passadas novas:
>   - `keywords[word]` refazia o mapa de palavras-chave (~30 inserções) a cada nome lido, e a leitura e checagem foi de 541 para 442 ms. Um mapa constante ainda é refeito onde é usado: falta a linguagem fazê-lo uma vez.
>   - A eliminação de subexpressões comuns criava um mapa novo a cada rótulo, salto ou chamada. Agora é uma tabela por função, com o número do trecho na chave: otimizar foi de 3.652 para 3.413 ms no `--release`.
> - **Medido e não mantido.** Um cache da última sala na arena, para `text += ...`: não mudou nada. Cada pedaço custa ~8 ns, que são a chamada ao runtime e não a busca da sala. O próximo passo seria juntar os pedaços de uma interpolação numa só chamada.
> - **Records (2,0× de C): medido em partes, sem ganho a manter.**
>   - **Onde está o tempo.** Build leva 47 ms. Nele, o coletor copia uma vez cada um dos ~1 milhão de arrays pequenos que sobrevivem, enquanto C só faz `malloc`. O rewrite leva 70 ms, em 19 milhões de visitas a ~3,7 ns cada. Depois da primeira volta o rewrite não aloca: cada `args` cresce uma vez só, porque `op` muda.
>   - **Crescer no lugar (`ins.args + [x]` sobre o array do elemento que morre na atribuição).** Implementado com regras conservadoras, piorou 2,3× num teste de crescimento repetido (20 → 46 ms). As salas dos arrays ficam numa tabela de tamanho fixo (1.024 baldes), feita para poucos vars que crescem. Com milhares de elementos, ela despeja entradas, e cada crescimento aloca o dobro. Para valer, a sala precisaria morar no próprio array (um cabeçalho de capacidade), o que muda o layout de todos os arrays. Revertido.
>   - **A checagem de fim de turno com o limiar pré-calculado.** Uma leitura por volta em vez de cinco, e nenhum ganho medido: as palavras da arena estão no cache L1. Revertido.
>   - **O cabeçalho de capacidade não serve.** Um valor de array pode apontar para o meio do seu buffer (`xs[a..b]`, as palavras de um texto), então a palavra antes dele não é de confiança.
>   - **O que serviu foi a tabela de salas crescer sob pressão.** Quando um balde cheio já despejou salas tantas vezes quanto há baldes, a tabela dobra. `items[k].tags.push(x)` em 50.000 elementos foi de 1.160 para ~210 ms (5,5×): antes, cada push que perdia a sala realocava o array inteiro. Os outros benchmarks ficam iguais, menos json, ~3% mais lento: com mais salas guardadas, carrega mais sobra. Uma tabela limitada a 2.048 salas o deixou 11% *mais rápido*, então há ganho em escolher melhor quais salas guardar.
>   - A tabela tem um teto de 16.384 baldes e volta ao tamanho que as salas restantes pedem a cada coleta. Sem esse teto, as salas de arrays que já pararam de crescer (os `kids` de 400.000 nós) a faziam crescer sem fim. Cada coleta a percorre inteira, e o teste "laços aninhados que fazem memória são lineares" passou de 4,4× para 6,2× no Mac do CI.
>   - Tentados e não mantidos: endereçamento aberto com marcas ou com deslocamento para trás (json 4–7% pior em todas as variantes); salas quentes e frias por coleta (counting 50% pior); crescer no lugar com a tabela nova (21 ms contra 19,5 ms de um array novo: juntar arrays pequenos já é barato).
>   - **O que falta é de desenho.** É preciso não copiar o que sobrevive com certeza (alocar direto no espaço velho quando o valor vai para uma estrutura velha) e ter arrays com cabeçalho de capacidade.

> **Concluído em 20/11/2026.**
> - **As decisões que dependiam da emissão.** O tamanho e o custo dos corpos (a política de inlining), o self por
>   endereço, mantê-lo na memória e trazê-lo aos registradores num laço só liam a árvore e os tipos: foram para
>   `ownership.qk` com os seus memos. `tools/parity.sh`: o mesmo IR em 229 de 229 programas.
>   - **Ficam na emissão, de propósito:** a checagem de fim de turno do laço lê o IR emitido porque é ele que diz,
>     exatamente, se o turno aloca; prever isso pela árvore repetiria tudo o que o lowering sabe sobre o que aloca.
>     A área da função é armazenamento que a emissão pede quando precisa.
> - **Último uso em ramos.** Quando o último comando que menciona o var é um `if` ou um `match`, cada ramo é
>   decidido como um bloco (recursivo); condição, sujeito e guardas são lidos antes. Uma linha empurrada numa lista
>   ou noutra conforme o ramo não é mais copiada: 4 cópias para 0 num laço, 48 → 38 ms; 3 dos 229 programas
>   mudam, o compilador entre eles.
>   - **Campos (`self.items` no fim):** não feito. Devolver um campo de `var self` deixa o mesmo valor no var de
>     quem chamou, que segue vivo; tomá-lo exigiria esvaziar o campo, uma mudança de semântica.
> - **Reutilização no lugar.** `xs = xs.map(f)` num var escreve cada valor onde estava o elemento, como
>   `xs[i] = f(xs[i])` faria (o buffer de um var é dele). Só quando `f` é um lambda que não lê `xs`, `xs` não foi
>   capturado, os elementos mantêm o tipo e não guardam memória (um valor novo num array velho pediria a barreira
>   do coletor). 50 maps de um milhão de `Int`s: 30 → 6 ms.
> - **Análise de escape.** Um literal de array de poucos elementos sem memória, num `let` lido só por índice (sem
>   fatia), por `len` ou por um `for`, nunca sai da função: nenhuma chamada o recebe, nenhum lambda o captura, não é
>   devolvido, guardado nem dado a outro nome. A memória dele é a área da função, feita uma vez por chamada e usada
>   de novo a cada volta: um laço que faz um por volta não faz nada para o coletor. 2,2 → 0,9 ns por volta, o mesmo
>   que sem array. Teste: `escapes` (os que saem: fatia, chamada, retorno, captura, outro nome, lista).

**Hoje:** `lower.qk` vai da árvore checada direto para instruções com registradores, decidindo no caminho
cópias, empréstimos, coleta de laço, C, closures e açúcar.
**Proposta:** três passos com uma IR tipada no meio (chamemos de MIR): (1) *desaçucarar* a árvore (interpolação,
`?`, `??`, `for`, padrões, métodos) em poucas formas; (2) *decidir memória* sobre a MIR (quem copia, quem empresta,
o que escapa, o que é único) como análises separadas e testáveis; (3) *emitir* a IR de máquina, mecânico.
**Por quê:** é a refatoração que destrava as outras otimizações de memória (análise de escape, reutilização no
lugar, pré-promoção, inlining do runtime), hoje inviáveis de encaixar num arquivo de 9.000 linhas. Era a "IR de
propriedade" do plano original, que ficou de fora.
**Risco:** o maior do plano; regressões sutis de desempenho. Mitiga-se mantendo o caminho antigo atrás de uma
opção e comparando o código gerado função a função até a paridade.
**Tamanho:** muito grande (o maior item). Pode ser fatiado: primeiro só o passo 1, que sozinho já encolhe o
lowering.

### B4. Remover o heap contado

> **Feito em 19/11/2026.** Saíram `runtime/heap.qk` e `runtime/counted.qk` (660 linhas), os ramos do lowering
> (`lower.qk` de 9.371 para 8.424 linhas), as opções `QUARK_COUNTED`/`QUARK_VALIDATE` e dez funções do runtime
> (as que vieram depois foram renumeradas). A alocação comum ficou sem a leitura e o desvio do heap.

**Proposta:** apagar `QUARK_COUNTED`/`QUARK_VALIDATE`, `runtime/heap.qk`, `counted.qk` e os 80 ramos do lowering,
guardando o trabalho numa tag do git. O que serviu a todos (empréstimo de `let`, `skipDesc`) já está no caminho
normal.
**Por quê:** perdeu a medição, falha em programas grandes, e cada mudança no lowering paga imposto por ele. Se
B3 existir e um dia a contagem voltar, ela volta como um passo sobre a MIR, muito menor.
**Tamanho:** médio, só remoção. Fazer **antes** de B3, para não migrar código morto.

### B5. O runtime como biblioteca inlinável

> **Primeiro passo feito em 19/11/2026: a alocação.** Uma alocação de tamanho conhecido (um registro, um nó, um
> array literal) é feita no próprio código do programa: o bloco atual da arena, o ponteiro avançado, a chamada ao
> runtime só quando o bloco acaba. `trees` caiu de cerca de 375 para 315 ms (16%); os outros ficaram iguais,
> dentro da variação que a disposição do código já dava (até 10% em laços apertados). O runtime não recebe esse
> caminho: as alocações dele são as suas. O resto (texto, mapas e arrays inlinados) continua o próximo passo.
>
> **Concluído em 20/11/2026.** Medido por operação antes de mexer (push 4,3 ns contra 1,4 ns de C; busca de chave
> `Int` 9,6 ns; de texto 15,2 ns; interpolação de três pedaços ~80 ns sob carga):
> - **`push`:** a sala do array é achada no próprio código (o mesmo hash de `growingRoom`, os quatro lugares do
>   balde); o `push` do runtime só quando a sala falta ou acabou. 4,3 → 2,3 ns.
> - **Busca em mapa com chave de um slot simples** (`Int`, `Bool`, caso sem dados): no código, como
>   `mapFindHashed` (o hash, os slots com etiqueta, a chave comparada). 9,6 → 3,4 ns.
> - **Chave de texto:** uma função do próprio programa, uma só, chamada de todo lugar (o hash de texto oito bytes
>   por vez, a sonda, comprimento e bytes comparados): colar em cada lugar aumentaria demais o código. 15,2 → 11,4
>   ns; `words` de 1,09× para 1,01× de C.
> - **Interpolação de textos e `Int`s** com três pedaços ou mais: uma só chamada (`rtInterp`) que mede, aloca uma
>   vez e escreve; os pedaços vão na área da função. ~22% mais rápida.
> - Comprimento e índice já eram código (o comprimento é um slot; o índice, uma checagem de limite e uma leitura).
> - **Não feito, e por quê:** guardar a IR do runtime e deixar o otimizador colá-la, como a proposta dizia. Os
>   caminhos rápidos são poucos e pequenos, e escritos no lowering ficam explícitos e testados; o layout que eles
>   leem (arena, mapa) está escrito ao lado de cada um, com as palavras do runtime.

**Hoje:** o runtime é compilado à parte e toda operação de texto, mapa e array é uma chamada opaca.
**Proposta:** como o runtime é Quark, guardar a IR das funções pequenas (comprimento, índice, `push` sem
crescer, `mapGet` de chave inteira ou texto, junção curta, alocação) e deixar o otimizador colá-las no programa,
especializadas pelo tipo. O caminho lento continua sendo chamada.
**Por quê:** é o maior ganho de desempenho que resta (`words`, `json`, o próprio compilador). Com B1, um built-in
com corpo em Quark já é inlinável pelo mesmo mecanismo.
**Risco:** tamanho do código; o runtime e o programa passam a compartilhar suposições de layout (precisa de um
número de versão de layout no cache).
**Tamanho:** grande; depende de B1.

### B6. Representação dos valores

Três mudanças de layout, que por mexerem no ABI interno devem vir juntas e antes do 1.0:

- **Textos curtos dentro do valor** (até 15 bytes nos dois slots, sem alocação).
- **Opcionais sem etiqueta onde há um valor impossível** (`Pointer?`, `Text?`, `[T]?`, handles: `None` é o zero),
  como já acontece na fronteira com C. Menos slots, menos cópias.
- **Mapas em Swiss table** (bytes de controle em grupos de 16), substituindo a tabela de índices com etiqueta feita
  nesta sessão.

**Por quê:** atacam o custo por elemento em todo programa de dados. **Risco:** cada uma toca runtime, lowering,
coletor e descrições de tipo; exigem a suíte e o teste diferencial de A3.

> **Medido em 20/11/2026, antes de começar.** A Swiss table rende pouco sobre a tabela que já existe: as entradas na ordem em que as chaves vieram, e um índice cujos slots trazem 32 bits do hash, de modo que uma sondagem que encontra outra chave segue sem ler a entrada dela.
> - Em `words`, o tempo de `mapFind` está no hash e na comparação das chaves de texto, não em sondar.
> - O ganho de B6 está nos dois outros itens. São mudanças de layout em todo o lowering e o coletor:
>   - textos curtos dentro do valor (sem alocar as palavras de um texto);
>   - opcionais sem etiqueta (`Text?` num slot a menos).
> - Nenhum dos três foi começado.

> **Textos curtos dentro do valor: feitos, medidos e desfeitos (20/11/2026).** Implementei em quatro etapas:
> - um texto de até 8 bytes guardado no próprio valor, marcado no bit de cima do comprimento;
> - as duas formas valendo para o mesmo texto;
> - a suíte inteira passando com eles ligados.
>
> Os benchmarks pioraram contra o código sem eles: json de 435 para 476 ms, words de 393 para 458 ms, strings de 348 para 380 ms. O custo das duas formas espalha-se pelos caminhos quentes:
> - o hash das chaves de mapa deixou de ser inlinado (10% de words);
> - cada função de texto do runtime decodifica o que recebe.
>
> O que a técnica poupa quase não existe aqui:
> - Swift e C++ a usam porque cada texto lhes custa um `malloc`/`free` e, no Swift, contagem de referências.
> - Em Quark alocar é avançar um ponteiro na arena, e fatias e palavras já são vistas de graça.
> - Em `strings`, nenhum dos textos curtos cabe em 8 bytes: todos têm de 9 a 15.
>
> Rust e Go também deixam a técnica fora da string padrão. Decisão: uma forma só. Ficaram as correções achadas no caminho:
> - `for c in f().codePoints()` lia memória já coletada; sua fonte agora é feita antes da marca do laço;
> - `tests/run/loopSourceMade` tinha guardado o valor errado;
> - `tests/run/textMix`, com textos e `Bytes` misturados.

> **Concluído em 20/11/2026.** Medido com cada executável num núcleo só e prioridade alta, alternando (a máquina
> estava carregada; assim a variação cai para ±5%).
> - **Opcionais sem etiqueta: feitos e mantidos.** `Text?`, `Bytes?` e `[T]?` não têm o slot do caso: são os dois
>   slots do valor, e `None` é o comprimento −1, que nenhum valor tem. Medido antes: registros com um `Text?`
>   eram 18% mais lentos que os mesmos registros sem a etiqueta. Com a mudança, 7% mais rápidos nesse teste; nos
>   benchmarks, `counting` −12%, `json` −2,5%, `records` −2%, os outros iguais.
>   - O layout está num lugar só (`Program.nicheDecl`, `caseLayout`). O lowering lê e escreve o caso por
>     `tagOf`, `optNone`, `optSome` e `optionAt` (um opcional que o runtime deixou na memória, `[caso][valor]`).
>   - A descrição do tipo é a do valor (`T`, `A...`): o coletor, a igualdade e o hash já tratam `(0, −1)` como
>     nada. Esquecer isso fazia o coletor ler o ponteiro como caso, e só aparecia quando um registro com `Text?`
>     era copiado (o `quark run` de um `extern` com biblioteca).
>   - Fora, de propósito: os textos de C (`CText?`), que já têm a forma da fronteira com C.
>   - Teste: `optionalsNoTag` (registros, arrays, mapas, canais, `receive`, `?`, `??`, `match`, e coletas de laço).
> - **Swiss table: feita, medida e desfeita.** Bytes de controle (7 bits do hash) e índices de 32 bits na mesma
>   alocação, oito bytes por vez (SWAR: o runtime não tem SIMD). Faltas ficaram 2× mais rápidas (14,4 → 7,6 ns),
>   mas acertos 18% mais lentos (13,6 → 16,0 ns): a tabela atual vai do índice, que já traz a etiqueta e a entrada,
>   direto à entrada; a Swiss passa pelo grupo, pela conta dos bits e por um índice à parte. Programas buscam
>   sobretudo chaves que existem (`countBy`, chaves de JSON). Fica a tabela atual.
> - **Textos curtos:** ficam fora, pelas medições acima.

### B7. Memória: copiar menos na coleta de laço

- **Pré-promoção:** o que é guardado num container de fora do laço é alocado direto na região de fora.
- **Análise de escape** (sobre a MIR de B3): o que não sai da função vai para a pilha ou registradores.
- **Reutilização no lugar:** atualizar campo ou elemento de um valor único nunca aloca.
- **Barreira de escrita filtrada no código gerado** (`remember` ainda custa 5 a 7% em `json`).

> **Promoção no lugar: feita, medida e desfeita (20/11/2026).** A pré-promoção por lugar não alcança o caso de `records`: os objetos são feitos dentro de `build()` e só depois vão para a estrutura de fora. A forma dinâmica alcança: quando a última coleta menor real guarda ao menos 60% do que as voltas fizeram, a próxima não copia, deixa os blocos onde estão e adianta o começo do que é jovem e a marca da arena. Uma em cada cinco copia, para medir de novo.
> - Ganhos: `records` de 117 para 100 ms e `trees` −4%.
> - Custo: até 40% de lixo fica guardado até a próxima coleta completa. Com mais memória nova tocada, o laço aninhado grande ficou superlinear: 6,5× localmente e **10,2× no Linux x86-64 do CI**, onde as faltas de página custam caro.
> - Com o limiar em 90%, que evita esse custo, nenhum benchmark promove.
> - O que falta para `records` é saber, ao alocar, o que vai sobreviver, sem pagar em memória. Seria uma região velha alocada por lugar com análise entre funções: `build()` devolve algo que vai para fora. Não uma heurística na coleta.

> **Medido em 20/11/2026: não vale.** O primeiro teste do `remember` foi posto no código: nenhum loop com marca, ou um container feito depois dela.
> - Nada mudou em `json` (440 → 450 ms), e `strings` ficou 2% pior pelo código maior em cada escrita.
> - As chamadas de `json` não saem cedo: são containers velhos mudados, e o custo está em anotá-los.
> - O que reduziria esse custo é não ter containers velhos apontando para novos: a pré-promoção acima.
>
> **Opcionais sem etiqueta (B6), analisados e deixados de lado.**
> - Ganho: um slot a menos por opcional, sem alocação nem trabalho poupado em laços. O caso quente dos benchmarks (`counts[w] ?? 0`) é um `Int?`, que não tem valor impossível.
> - Custo: tocar construção, `match`, `??`, `?`, igualdade, hash, descrições e o coletor.

Meta: `records` de 2,2× para menos de 1,5× o C.

> **Rodada de 20/11/2026: de 1,95× para cerca de 1,6× o C.** Medido por partes (cada executável num núcleo, prioridade
> alta): sem coleta nenhuma, `records` leva 76 ms (1,18× o C); só com as menores, 116 ms; com as completas também,
> 151 ms. As cópias do coletor são quase toda a diferença.
> - **Coletas completas que esperam o que precisam.** Uma completa copia de novo tudo desde a marca; quando ela
>   guarda quase tudo o que olhou (dados que crescem), a próxima espera o dobro, até 8 vezes o que ficou vivo; quando
>   acha muito lixo, a espera volta a cair. Com limite de memória (`--memory`), fica em 1. `records` −10%.
> - **Valores pequenos copiados onde são achados.** Um array de valores simples ou um texto de até 64 bytes é
>   copiado sem a tabela do que já foi copiado (dois donos ganham uma cópia cada, de poucas palavras: valores não têm
>   identidade). Cada cópia fazia três buscas nas faixas jovens, a tabela de compartilhamento e a de salas. A fase de
>   construção de `records` foi de 51 para 34 ms.
> - Juntos: `records` 136,6 → 111,6 ms (−18%), `json` −3%, `sorting` −3%, os outros iguais.
> - **De passagem:** o literal de um lado de `xs + [x]` é da área da função (a junção copia os dois lados).
> - **Não alcançado: menos de 1,5×.** Falta a pré-promoção por chamada: o que `build()` faz vai para `fns`, fora do
>   laço, e é copiado uma vez pelas coletas menores do laço de `main` (cerca de 20 ms). Ela pede uma região "guardada"
>   na arena que a alocação no código use como um bloco comum, e que os laços de dentro da chamada marquem e coletem
>   sem confundi-la com os blocos da arena. As formas limitadas que pesei arriscam memória sem limite, quando a função
>   chamada tem um laço longo próprio. Fica como próximo passo, com esse desenho.
>
> **Rodada de 21/11/2026: medido por dentro, 1,61× → ~1,52× o C** (o melhor de 20 execuções alternadas, num núcleo:
> C 65 ms, `records` 99 ms).
> - **Sem coleta nenhuma: 72 ms** (1,07× o C). A coleta custa ~28 ms.
> - **O programa inteiro tem seis coletas:** uma completa copia 39 MB, e cinco menores copiam 64 MB, quase tudo os
>   `args` novos achados pelos cartões dos arrays velhos (`lookAgain`).
> - **Experimento: não copiar de novo o que os laços de fora já copiaram.** O que é achado por um recipiente velho
>   foi para um bloco que nenhuma coleta vê como novo (de propósito vazando, só para medir). Ganhou 2,5%. O custo não é
>   a cópia repetida entre níveis: é a primeira cópia do que sobrevive.
> - **Feito:**
>   - a barreira de escrita (`remember`) lembra a última entrada, e uma escrita de novo no mesmo array só carimba o
>     cartão (sem a busca na tabela);
>   - a cópia de um array pequeno de valores simples aloca na hora no sobrevivente e copia palavra por palavra (sem
>     `survive` nem `memcpy`).
>   - Juntos, ~2–3%.
> - **O que falta para 1,5× com folga** é tirar a primeira cópia. Para isso, o que um laço guarda num recipiente velho
>   precisa ser alocado já na área do laço ativo mais de fora (a pré-promoção por chamada). Isso pede marcar cada saída
>   de laço no lowering (`break`, `return`, `?`, pânico), para o runtime saber quais laços estão ativos. Sem isso, o
>   lixo no que é mais velho que todos os laços ativos nunca seria coletado. É o desenho acima, ainda não feito.
>
> **Rodada 2 de 21/11/2026: 1,66× → 1,46× o C (meta do 0.12 alcançada).** Medido pelo `tools/bench.sh` (o melhor de
> três) e pelo melhor de dez execuções intercaladas (`records` 82,8 ms, C 58,6 ms: 1,41×). Medido também por fase:
> montar 29 ms (C 20,5), reescrever 66 ms (C 37). A distância maior era o código do laço, não a coleta: em regime, com
> as coletas já feitas, 1,15 ms por passada contra 0,69 ms do C.
> - **A coleta completa espera mais quando os dados só crescem.** Se as menores desde a última completa guardaram 2/3
>   do que os turnos fizeram, ou mais, o piso da primeira completa sobe de 16 para 64 MB. A completa de 39 MB da
>   montagem, que não devolvia nada, sumiu. Um laço que faz lixo continua com o piso de 16 MB. −6 ms.
> - **A checagem do turno, de cinco leituras encadeadas a uma.** O endereço da arena e o prazo da próxima coleta
>   (marca + limite) são calculados uma vez, antes do laço, em registradores. O prazo é recalculado só depois de uma
>   coleta. −3 ms.
> - **`x % d == 0` sem arredondar para baixo.** O resto truncado é zero exatamente quando o arredondado para baixo é:
>   a divisão por constante leva a marca `divTruncated` e pula a correção de sinal (x86-64, ARM64; o WebAssembly a
>   ignora, e continua certo). −4%.
> - **Afundar nos ramos** (`sinkIntoBranches`, tier otimizador): uma instrução pura cujo valor só um lado de um desvio
>   lê desce para esse lado. Os quatro campos de `ins` que só o ramo que reescreve usa deixaram de ser lidos e
>   guardados na pilha em cada volta. As junções (phis) que nada lê não contam como uso. −4%.
> - **Medido e desfeito:** calcular `i × 40` uma vez por elemento (em vez de um `imul` por campo) ficou mais lento, pelo
>   registro a mais; esperar mais nas coletas menores que guardam 2/3 não mudou nada.
> - `json` também ganhou (489 → 460 ms); os outros ficaram iguais, dentro do ruído.

### B8. O grafo sem JSON por dentro

O código do grafo monta e lê JSON e converte para QGN na borda. Trabalhar sobre os valores de `std.graph`
simplifica `graph.qk`/`collab.qk` (4.000 linhas) e deixa programas Quark manipularem grafos com tipos.

> **Feito em 20/11/2026.**
> - **O escritor faz os valores.** `graph.qk` monta `Graph`, `Decl`, `Node`, `Pin`... em vez de texto JSON
>   concatenado; o leitor faz a árvore sintática a partir deles (`programOfGraph`). Nenhuma linha de JSON ficou em
>   `graph.qk`: o JSON que ele tinha, próprio, foi para `collab.qk`, o único que ainda o usa.
> - **As duas notações saem dos valores** (`compiler/graphio.qk`): um escritor só, com a sintaxe de cada uma, os
>   campos de cada registro na sua ordem e os que têm o valor de quando são omitidos, omitidos; JSON lido para os
>   valores (e QGN por ele). Um grafo dado em QGN ou JSON é dito de novo pelos valores, então as duas notações de um
>   grafo sempre batem. O formato JSON documentado (as chaves `type`, `fn`, `else`...) não mudou.
> - **O que os valores não diziam:** um `else {}` vazio e nenhum `else` eram o mesmo `Block()`. `Node.otherwise`
>   passou a `Block?` (em `std.graph` também).
> - **O compilador tem a sua cópia dos tipos** (`compiler/graphvalue.qk`): ele é compilado por um compilador anterior
>   (na CI, a última release), cujo `std.graph` pode ser outro. O teste que compila o QGN de um grafo como constante de
>   `std.graph` mantém os dois iguais.
> - **`collab.qk` fica sobre a árvore genérica, de propósito.** O diff e o merge trocam os ids por chaves dentro da
>   árvore, acham fios e blocos em qualquer campo e comparam nós pelo texto canônico: um algoritmo estrutural, que
>   a forma genérica deixa curto. Com registros, cada caminhada viraria código campo a campo. A borda dele já são os
>   textos dos grafos, lidos e escritos pelos mesmos valores.
> - De passagem: o JSON da LSP e o do grafo eram duas implementações; o grafo passou a usar o de `json.qk` (o layout)
>   e `floatG` foi para `format.qk`.

### B9. Concorrência: do protótipo ao modelo

- **`Channel<T>` tipado** (o runtime já copia qualquer valor entre arenas). **Feito:** `channel`, `send`,
  `receive` e `closeChannel` são genéricos no prelúdio; o `T` vem de onde o valor vai
  (`let c: Channel<Int> = channel()`): a dica de tipo agora diz os tipos que os valores de qualquer chamada genérica
  não dizem. O canal guarda o tamanho e a descrição do seu `T`; `send` passa o endereço dos slots. Os valores na fila
  ficam numa arena dos canais (o remetente pode acabar antes), copiados para ela ao enviar e para a memória de quem
  recebe ao receber (`copierFresh`: as cópias como alocações comuns da arena, jovens para os laços). A arena dos
  canais esvazia quando todas as filas esvaziam, e é refeita a partir do que está na fila quando dobra (memória
  limitada: 2 milhões de envios com 100 na fila, 24 MB de pico). Um copiador só para os canais, mantido entre
  valores: com um novo a cada valor, e a tabela de cópias de 32 KB zerada a cada um, 2 milhões de pares
  envio/recebimento de textos levavam 1.265 ms; mantido e com a tabela pequena, 185 ms (os canais só de texto, com
  `malloc`: 139 ms). Testes: `channelsTyped` (registros, tuplas, enums, `Float`, funções, mapas, canais de canais,
  interfaces, `Unit`, opcionais, resultados).
- **Tudo o que espera é cooperativo:** `connect`, resolução de nomes, `runProcess`, leitura do teclado.
  **Feito:** numa tarefa, essas chamadas rodam numa thread própria (`offThread` em `runtime/tasks.qk`) e a tarefa
  devolve a vez até a thread marcar o fim; fora de uma tarefa, são feitas ali mesmo. Um mecanismo só para
  `connect` e `listen` (nome e conexão), `runProcess`, `readLine` e a leitura da entrada: a thread roda com uma cópia
  do contexto da tarefa que espera (sem ela, o `connect` do winsock caía: o código lê o contexto), e a chamada toca
  só a sua tarefa (memória com `malloc` e a da tarefa, que não roda enquanto isso). O
  agendador não é acordado pela thread (um pipe não seria socket no Windows, e `WSAPoll` só vê sockets): enquanto
  uma tarefa espera uma, ele olha de novo a cada 2 ms; ao ver o fim, junta a thread (o que ela escreveu fica
  visível). `connect` sem bloquear e com espera pelo socket seria a forma nativa, mas a resolução de nomes
  bloqueia de qualquer jeito, e uma thread por conexão custa dezenas de microssegundos diante da rede. Medido:
  três processos de 0,5 s em três tarefas, 0,54 s no total, e as outras tarefas seguem; teste em `process.qk`.
- **Cancelamento e prazos:** `timeout(seconds, f)`, fechar um canal acorda quem espera. **Feito:** `go` dá a
  `Task`; `cancel(task)` a para onde ela devolve a vez, como um pânico (sem `defer`, a memória liberada), ou já, se
  não está rodando; esperando uma chamada numa thread, só quando a chamada acaba (ela escreve na memória da tarefa).
  `withTimeout(seconds, f) -> T?` roda `f` como tarefa de fundo e recebe o valor por um canal com prazo
  (`receiveWithin`); no fim cancela a tarefa e fecha o canal. Um canal fechado e vazio é liberado: o número dele
  leva uma geração (bits acima do 32), e um número antigo age como um canal fechado e vazio; assim 2.000
  `withTimeout` seguidos não deixam canais para trás. O agendador respeita o prazo de quem recebe (a espera no
  `poll` vai até ele, e sem tarefas que enviem a espera acaba nele em vez de um impasse). Fechar um canal já
  acordava quem esperava. Teste: `cancel`. De passagem: as capacidades (`--allow`) passaram a ser conferidas antes
  de escolher o template, porque desde `Channel<T>` os built-ins genéricos (`send`, `receive`) escapavam da
  conferência na chamada; as chamadas do próprio prelúdio não são conferidas, e um built-in com corpo em bloco
  (`withTimeout`) tem o grupo do primeiro built-in que chama.
  No macOS, 2.000 `withTimeout` levavam 3 a 6,5 s (58 ms no Windows): medido por partes na CI, o custo era a
  primeira alocação de cada tarefa, um bloco de 16 MB com `calloc`, que o macOS zera inteiro ao reaproveitar a
  região recém-liberada. Agora o bloco novo de uma arena é do tamanho de tudo o que ela já reservou, de 64 KB a
  16 MB: tarefas duas vezes mais rápidas no Windows também, e os benchmarks iguais.
- **Concorrência estruturada:** um escopo que só termina quando as tarefas que abriu terminam, e propaga o pânico
  com a origem (`go` solto fica para servidores). **Feito:** `scope(s => ...)` roda o corpo e espera toda tarefa
  que `s.start(f)` fez nele (e as que elas começam nele); dá o valor do corpo. Uma tarefa que pare o programa diz
  a linha que a começou (`... (in the task started at line 5)`). Escopos aninham, dentro de tarefas também; quem
  espera um escopo numa tarefa devolve a vez até as dele acabarem. `start` depois do fim do escopo para o
  programa. De passagem, três correções da linguagem: `s.start(...)` com um local `start` que não é função chama
  a função (antes, "is a value"); `runtime(...)` num corpo em bloco do prelúdio tem o tipo escrito onde vai, ou
  nenhum; e as linhas de um lambda dado a um built-in são as do programa (antes, a da chamada do built-in).
  Testes: `scopes`, e os pânicos `scopeTask` e `scopeEnded`.
- **Decisão a tomar e escrever:** uma thread por agendador (hoje) ou um agendador por núcleo com roubo de trabalho.
  Recomendo ficar em uma thread no 1.0 e oferecer `serve(..., workers: n)` como processos ou threads isoladas que
  compartilham o socket: mantém o modelo "tarefas não compartilham memória" e escala em núcleos.

### B10. Compilação incremental de ponta a ponta

Cache por módulo e por conteúdo para parse, checagem e MIR (o de otimização já existe), lowering em paralelo por
função, threads que não recarregam o código (o custo de 10 ms). Meta: rebuild do compilador abaixo de 0,5 s e
`quark run` de um programa pequeno abaixo de 30 ms.

> **20/11/2026: `quark run` de um hello de 38,5 para 20,6 ms** (medido com `Process.Start`, sem o Git Bash, que
> sozinho põe 30 ms em cada processo). O tempo estava em saber se o runtime guardado no cache servia: o compilador lia
> o próprio executável (16 MB) e o resumia a cada execução, 15 a 20 ms. Agora a identidade dele é a soma das suas
> fontes (`compilerSources`, escrita por `compiler/embed.sh` com `cksum`): o runtime depende só delas, e dois
> compiladores feitos das mesmas fontes fazem o mesmo (o ponto fixo do bootstrap). Do que resta, 15 ms são abrir o
> executável; compilar o hello leva 2 ms.
>
> **O rebuild do compilador continua em ~2,6 s** (ler e checar 0,66 s, lowering 0,85 s, otimizar 0,6 s, código de
> máquina 0,45 s). Mesmo com as 1.810 funções tiradas do cache de otimização, otimizar leva 0,6 s: cada acerto codifica
> a função, a resume e decodifica o resultado. Chegar a 0,5 s pede guardar por módulo a leitura, a checagem e o
> lowering, e a checagem hoje é do programa inteiro (genéricos instanciados entre módulos): um trabalho próprio.
>
> **21/11/2026: as partes por função em threads.** O otimizador rápido (`quickOptimize`) e o código de máquina
> (x86-64 e ARM64) passaram a rodar nas threads do processador quando o programa tem 20.000 instruções ou mais.
> - O código de máquina é feito em 16 partes, cada uma a partir do byte 0. Depois as partes são juntadas, com os
>   deslocamentos de patches, relocações, chamadas, linhas e frames corrigidos. Os bytes saem iguais aos de antes: cada
>   função continua na sua fronteira de 16 bytes, e o ponto fixo se mantém.
> - O compilador compilando a si mesmo no tier rápido: otimizar de 524 para 180 ms, código de máquina de 450 para
>   155 ms, o total de 2,4 para 1,75 s.
> - O que sobra é ler e checar (0,6 s) e o lowering (0,8 s), que são do programa inteiro. Chegar a 0,5 s continua
>   pedindo guardá-los por módulo. Isso é uma arquitetura nova: as instâncias de genéricos, os textos do bloco de dados
>   e as decisões de inlining cruzam os módulos. Não foi feito.
>
> **21/11/2026: começado. Medido por dentro e o primeiro passo feito.**
> - **Onde está o tempo** (o compilador compilando a si mesmo, ~1,75 s de relógio): ler 0,2 s, constantes 0,12 s,
>   checar 0,25 s, lowering 0,8 s (aliases 79 ms, modificação no lugar 47 ms, as funções uma a uma 604 ms, o resto
>   ~70 ms), otimizar 0,17 s, código de máquina 0,15 s.
> - **E a memória:** 3,5 GB no pico; 1,1 s de CPU no kernel, em falhas de página (memória nova zerada pelo sistema).
>   No perfil, `memcpy` são 22% da checagem, e cópias inteiras do array de expressões aparecem em `loadProgram`,
>   `checkFile`, `runConsts` e `lowerFrom`.
> - **Feito: o cache de constantes.** As constantes deixaram de ser compiladas e executadas de novo a cada build.
>   - A chave resume o texto canônico de cada declaração que elas alcançam, todos os tipos do programa (o número de
>     um caso e o lugar de um campo estão no despejo), os nomes delas, o compilador e a máquina.
>   - O despejo de cada uma é guardado por arquivo e nome (`quark-consts-*.bin`). Num acerto, os despejos são só lidos
>     de volta.
>   - Ler e checar: 0,62 → 0,50 s. O executável sai com os mesmos bytes, com o cache frio ou quente.
> - **O que falta, em ordem de ganho por risco:**
>   1. O lowering das funções em threads (604 ms → ~90 ms). Cada thread baixa uma faixa das funções, e as tabelas
>      compartilhadas são renumeradas ao juntar: dados, externs, assinaturas C, tabelas de interface, auxiliares,
>      entradas C, chamadas embutidas. `asValue` já é decidido antes, então nada depende da ordem.
>   2. Cortar as cópias inteiras da árvore e a memória que nada devolve (3,5 GB), que custam `memcpy` e falhas de
>      página em todas as fases.
>   3. O cache por módulo (leitura, checagem, lowering), para a meta de 0,5 s: os passos 1 e 2 levam a ~1,2 s; abaixo
>      disso, só não refazendo o que não mudou.

## 4. Fase C: a linguagem (decidir antes de congelar)

> **Decidido e feito em 19/11/2026**, pela regra "uma forma canônica para cada coisa; inferência quando não custa
> nada; o mais moderno e eficiente":
>
> - **Constantes em padrões**: feito. O nome de uma constante visível compara (`limit => ...`); qualquer outro nome
>   liga o valor. Como em Rust, sem exigir maiúsculas.
> - **`T?!`**: feito. Um resultado cujo valor pode faltar; `Ok(None)` toma o tipo do destino.
> - **`a..=b` como intervalo**: decidido que **não**. `a..b` continua a única forma de intervalo de valores; `..=`
>   fica nos padrões, onde não há outra. Duas formas para o mesmo intervalo contrariam a forma canônica.
> - **`Char`**: decidido que **continua `Int`** (como a `rune` do Go): custo zero, e aritmética de bytes e de
>   caracteres (`c - 'a'`) é o uso comum, no runtime e nos programas. `codePoints`, `char(n)` e os padrões
>   `'a'..='z'` cobrem o resto.
> - **Encerramento de recursos**: decidido que **`defer`** basta (uma forma só, como em Go e Zig); sem `using`.
> - **Efeitos visíveis**: feito no `quark doc` (o `effect` inferido aparece na assinatura).
> - **Edições**: feito. `edition` no `quark.toml` é lido; outra edição que não a do compilador é um erro claro.
> - **Ficam para depois** (maiores, e somam sem quebrar): conversão automática de erros no `?`, `Self` e
>   implementações padrão em interfaces, `pub` por campo.
>
> **Feitos em 20/11/2026**, os três, somando sem quebrar nada:
> - **O `?` convertendo erros.** Quando o erro da função é um enum com exatamente um caso que guarda o tipo do erro
>   e nada mais, o `?` o põe nesse caso (`enum AppError { Parse(e: ParseError), Io(why: Text) }`: o padrão do
>   `#[from]` do Rust, sem anotação); quando a função dá um `T!`, o erro vai como texto. Dois casos possíveis são
>   um erro que manda dizer qual com `mapErr`. Um traço como `From` foi pesado e deixado: a conversão por caso é a
>   forma canônica que já se escreve com enums, e não pede uma segunda maneira.
> - **Implementações padrão e `Self` em interfaces.** Uma função de interface com corpo é o padrão: um tipo que
>   tem as outras e não esta ganha `fn Tipo.nome(self, ...)` com esse corpo, escrito como texto e lido como do
>   programa (no arquivo da interface), então é checado e chamado como qualquer outra função do tipo. `Self` no tipo
>   de uma função é o tipo que serve: `fitsMember` o troca; uma interface com `Self` é uma restrição
>   (`T: Ordered`), não um valor, como no Rust.
> - **`pub` por campo**, dentro do tipo opaco que já existia: o campo `pub` é lido e mudado em qualquer lugar; os
>   outros, a construção e a desestruturação ficam do módulo. Num tipo que não é opaco todo campo já é público, e
>   `pub` num é um erro (uma forma só). No grafo, `Field.isPub` (`"pub"` no JSON).
> - Testes: `errorConversion`, `interfaceDefaults`, `pubFields`, e os erros `errorTwoCases`, `errorNoCase`,
>   `selfInterfaceValue`, `privateField`, `pubFieldNotOpaque`.

Cada item é uma decisão que o 1.0 torna cara de mudar.

| Item | Proposta |
| --- | --- |
| Constantes em padrões | `match b { bOpenFile => ... }` compara com a constante; um nome novo que capture tudo passa a exigir `let x` ou minúscula não declarada com aviso |
| `T?!` | aceitar resultado de opcional (e `(T?)!`), ou documentar o padrão do campo `error`; recomendo aceitar |
| `a..=b` como intervalo | hoje só em padrões; o erro é natural demais para ficar |
| Erros tipados | `T ! E` existe; falta a conversão automática de `E1` para `E2` no `?` (um `From`), e `Err` com contexto (arquivo, linha, causa) |
| Interfaces | `Self` em interfaces, implementações padrão, restrições em genéricos (`T: Compare`) declaradas e não só inferidas |
| `Char` | hoje um caractere é um `Int`; decidir se vira tipo (recomendo: sim, distinto de Int, sem custo) |
| Visibilidade e módulos | `pub` por campo, reexportação, módulos internos de um pacote |
| Encerramento de recursos | `File`, `Socket` fechados por `defer` hoje; avaliar `using f = openFile(...)` |
| Efeitos | a regra "inferido, declarado só no público" está boa; falta mostrar na doc e no LSP o conjunto de efeitos |
| Edições | implementar de fato `edition` no `quark.toml` e o caminho de depreciação do STABILITY.md, **antes** de precisar dele |

## 5. Fase D: backends e sistema

> **Estado em 19/11/2026:** nada desta fase foi feito ainda. ARM64 em registradores, `libquark` no macOS e a suíte
> em wasm precisam de uma máquina ou VM para serem verificados, e não há nenhuma aqui. Escrever `Float` sem o
> `snprintf` do C (Ryū ou Dragon4) é o primeiro item que dá para fazer e verificar aqui: precisa reproduzir byte a
> byte a saída atual, comparada contra o C em milhões de números.

- **ARM64 em paridade:** argumentos em registradores, SIMD NEON, e uma máquina ou VM no ciclo.
> **Feito em 20/11/2026: escrever `Float` sem o C.** `floatText` (runtime/base.qk) acha os dígitos mais curtos que voltam ao mesmo número com inteiros grandes exatos: a expansão decimal do `double`, o arredondamento para *d* dígitos (metade para o par) e a volta decidida contra o intervalo de arredondamento, empates incluídos.
> - Antes eram até 34 chamadas ao C por número (`snprintf` e `strtod` para cada precisão de 1 a 17). Agora é 2× mais rápido.
> - Comparado com o antigo em 200 mil números de todo tipo, a saída é igual, menos 230 empates exatos (`1000000000000000.25`). Neles o `printf` do Windows arredondava para cima e o da glibc para o par, então a saída do Windows e a do Linux diferiam. Agora é a mesma em todo sistema (`tests/run/floatText`).
> - **Ler um `Float` também, sem C** (`textFloat`). Aceita o que o `strtod` aceitava no texto inteiro: ponto, expoente, `inf`/`infinity`, `nan`/`nan(...)` e hexadecimal (`0x1.8p3`).
>   - Até 15 dígitos com expoente até ±22 é exato com `Float`s.
>   - Fora disso, um palpite é corrigido por comparação exata com os números a meio caminho, usando os mesmos inteiros grandes.
>   - Guarda 800 dígitos, mais a marca de que algum depois deles não é 0. Nenhum meio caminho tem mais dígitos que isso.
>   - Comparado com o `strtod` do C, bit a bit, em 1,2 milhão de textos de todo tipo (meios caminhos exatos de 55 dígitos, as bordas dos subnormais, hexadecimal): nenhuma diferença (`tests/run/floatRead`).
>   - O runtime não declara mais o `strtod`.

- **Sem libc onde der:** Ryū para escrever `Float`, parser próprio para ler, memória direto do sistema. No Windows
  e no Linux o executável passa a depender só do kernel; no macOS a `libSystem` é obrigatória.

> **Feito em 21/11/2026: sem libc onde dá.**
> - **Todo sistema.** `{x:.2}` não usa mais o `printf` do C: `fixedText` (runtime/base.qk) arredonda a expansão
>   decimal exata metade para o par, igual em todo sistema. Comparado com o Python em 3.000 valores e precisões de 0 a
>   12, não houve diferença. `parseInt` é Quark (antes era `strtoll` com `errno`). O runtime não chama mais
>   `snprintf`, `vsnprintf` nem `strtoll`.
> - **Windows: só o kernel.** Um executável importa `kernel32.dll` e `ntdll.dll` (`memcpy`, `memmove`, `memset`,
>   `memcmp`, `strlen`, as do próprio sistema), e nada mais:
>   - o heap (`malloc`...) é o do processo (`HeapAlloc`);
>   - os arquivos são `CreateFileW`/`ReadFile`/`WriteFile` (um `FILE*` é uma palavra do runtime);
>   - o ambiente, a hora e o fuso vêm do kernel32 (`SystemTimeToTzSpecificLocalTime` resolve o horário de verão da
>     data);
>   - a linha de comando é lida pelas regras do `CommandLineToArgvW` (sem o shell32);
>   - o Winsock é carregado no primeiro socket (`LoadLibraryW`, `GetProcAddress`), então um programa sem rede não o
>     carrega.
>   - Na libquark esses nomes C são do runtime e não são exportados: o host continua com a sua biblioteca C.
> - **Linux: a libc fica.** Ficar sem ela pediria um resolvedor de DNS (`getaddrinfo`), a base de fusos (`localtime_r`
>   lendo TZif), threads por `clone` com armazenamento por thread próprio, e perderia o `dlopen` que as chamadas a C
>   de um programa usam. É desproporcional para o "onde der". O executável já pode usar a musl (`--libc musl`).
> - **macOS:** a `libSystem` é obrigatória, como previsto.
- **Informação de depuração padrão:** DWARF e PDB/CodeView, para depuradores e perfiladores externos.

> **Feito em 21/11/2026: ARM64 em paridade.** As chamadas entre funções Quark passam os oito primeiros slots em x0–x7, como o x86-64 faz com cinco registradores.
> - Nenhum valor tem x0–x7 por casa, então os movimentos de entrada e saída vão em qualquer ordem.
> - O código do compilador ficou 8,8% menor (21,78 → 19,86 MB) e o da libquark 13% menor.
> - O NEON já cobria as mesmas operações vetoriais do x86-64.
> - Verde no Linux ARM64 e no Apple silicon, com o ponto fixo e a suíte inteira.
>
> **Feito em 21/11/2026: informação de depuração (`-g`).** `compiler/dwarf.qk` escreve DWARF 4 (`.debug_info`, `.debug_abbrev`, `.debug_line`, `.debug_frame`) e CodeView (`.debug$S`).
> - Os backends guardam onde começa o código de cada linha e cada prólogo, e só com `-g`.
> - `quark object -g` põe isso no objeto: ELF, Mach-O (`__DWARF`) e COFF. No COFF vão DWARF, para as ferramentas GNU, e CodeView, de que o `lld-link /debug` faz um PDB.
> - Verificado com `llvm-dwarfdump --verify`, `llvm-pdbutil` e o gdb: no Windows, num host C ligado com o objeto, `break greet`, `next` e `bt` funcionam nas linhas Quark.
> - O executável carrega o programa ao começar. Com `quark build -g`, o compilador põe no payload um ELF com o DWARF, e o runtime o registra pela interface JIT do gdb (`__jit_debug_descriptor`, `__jit_debug_register_code`). Os executáveis agora têm tabela de símbolos (COFF, ELF `.symtab`, Mach-O `LC_SYMTAB`) com as funções do runtime e esses dois nomes.
> - `quark run -g` não tem isso: o runtime do `quark run` é mapeado na memória, sem símbolos que o gdb ache; o depurador próprio (`quark debug`) cobre esse caso.
- **wasm32:** rodar a suíte num runtime wasm; `std` sem sistema bem delimitada.

> **Feito em 21/11/2026: wasm32.** `quark build --target wasm32-wasi` faz um módulo WebAssembly (WASI preview 1) com
> o programa e o runtime juntos.
> - **O backend** (`compiler/wasm.qk`, escrito byte a byte, sem ferramentas):
>   - Cada registrador virtual é um local do seu tipo. Entre funções, todo slot é um i64, como os outros backends
>     guardam 64 bits num registrador.
>   - Os saltos do IR viram segmentos dentro de um `loop`, e o `br_table` vai ao segmento da vez.
>   - O endereço de uma função é o seu número na tabela. Há uma parte da tabela para cada quantidade de argumentos das
>     chamadas por valor: uma função com nome recebe um adaptador que deixa de fora o ambiente.
>   - Uma função que o C chama tem um thunk com a forma única do C aqui (quatro i64). A que tem o endereço tomado ganha
>     uma entrada por slots, para o `parallelMap` numa thread só.
>   - O `memcpy` e o `memset` são `memory.copy` e `memory.fill`.
>   - Só entra o que o programa alcança a partir de `_start`: um hello tem 247 KB.
>   - A seção `name` faz o wasmtime mostrar os nomes das funções numa armadilha.
> - **O runtime** (`runtime/wasi.qk`):
>   - O heap é próprio, por faixas de tamanho, crescendo a memória.
>   - Arquivos e pastas vão pelas pastas que o motor dá (as preopens de `--dir`). Também há relógio, ambiente e
>     argumentos.
>   - A matemática é a do fdlibm (`runtime/wasimath.qk`): a até um ulp da glibc em 194 mil pontos.
>   - Um pânico termina com a mensagem e o status 1.
> - **A suíte** (`tools/wasmtest.sh`, no CI em `.github/workflows/wasm.yml`): 124 programas de `tests/run` e
>   `tests/panics` com a saída gravada, no wasmtime, todos verdes.
> - **O que fica de fora**, com o porquê em `tests/wasm-skip.txt` (12):
>   - tarefas em pilhas próprias: o wasm tem uma pilha só, e a troca de pilhas ainda não é padrão nos motores;
>   - outros programas e sockets: o WASI preview 1 não tem;
>   - as bibliotecas do C;
>   - a pilha do motor: recursão funda demais vira armadilha dele;
>   - o rastro de chamadas de um pânico, lido dos frames da máquina.
> - **De passagem:**
>   - um validador de wasm local para depurar sem motor (fora do repositório);
>   - `fixedText` e `parseInt` em Quark, que serviram aos dois lados.
- **libquark no macOS** e a API C revisada uma última vez antes de congelar.
- **Executáveis menores:** só o que o programa usa do runtime (hoje vai inteiro).

> **Medido em 20/11/2026.** Um "hello" tem 842 KB, dos quais 838 KB são o runtime. A imagem do runtime é guardada em cache uma vez por sistema, e o programa vai junto, carregado pelo runtime ao começar. A tabela de funções do runtime é preenchida com todas elas. Para deixar de fora as não usadas, é preciso compilar o runtime por programa, com alcançabilidade a partir das chamadas do programa e uma tabela parcial, e guardar o cache pelo conjunto de funções usadas.

> **Feito em 20/11/2026: executáveis menores.** `quark build` compila o runtime por programa, só com o que ele alcança.
> - O conjunto de funções da tabela do runtime que o programa usa vem das chamadas das funções alcançadas a partir de `main`, mais as relocações dentro delas.
> - O runtime é compilado com essa tabela parcial (`LowerOptions.runtimeUsed`). As funções que nenhuma raiz alcança viram um `ret` (`IrProgram.keepReachable`). As raízes são a entrada do sistema, as tabelas de interfaces, os endereços de função e as chamadas por nome a uma função exportada.
> - O cache é guardado pelo hash do conjunto usado. Montar o runtime de um conjunto novo leva 0,24 s; com o cache, 33 ms.
> - Tamanhos: hello 911 → 136 KB, json 979 → 639 KB, words 916 → 532 KB, background 918 → 367 KB.

## 6. Fase E: biblioteca e ferramentas

> **Feito em 19/11/2026:** `std.csv`, `walkFiles` no `std.files`, `quark test --filter`, e o tutorial
> ([TUTORIAL.md](TUTORIAL.md), dez passos, seus exemplos verificados pela suíte). Antes disso já tinham entrado
> `std.encoding`, `std.hash`, `std.args`, `std.files`, `std.net` e `std.http`.
>
> **E concluída em 20/11/2026:**
> - **TLS** pela biblioteca do sistema, num motor de bytes por sistema dentro do runtime (que já é compilado por
>   alvo): Schannel no Windows, OpenSSL carregada ao primeiro uso no Linux (BIOs de memória), SecureTransport no
>   macOS. O std vê quatro funções (`tlsConnect`, `readTls`, `writeTls`, `closeTls`); `connectTls` em std.net e
>   `https://` em std.http. O caminho OpenSSL foi testado também no Windows, com a libssl do msys64.
> - **JSON tipado**: `encode`/`decode` feitos pelo checker para cada tipo usado.
> - **Subprocessos em fluxo**: `startProcess`, `writeProcess`, `readProcess`, `waitProcess`, `killProcess`.
> - **Fusos com nome**: os TZif do sistema, ou a regra de hoje de cada zona (std.zones, do tzdata), conferidos
>   contra o zoneinfo do Python (960 de 960).
> - **Pacotes**: versões semânticas com seleção mínima de versões, e registro (um repositório git com `index.toml`).
> - **LSP**: renomear e referências entre arquivos, ir para implementações, efeitos no hover, edições por trecho,
>   textos abertos vistos pelos outros arquivos, verificação guardada.
> - **Testes**: `quark test --coverage`; `forAll` e geradores com redução em std.testing.
> - **Guias** (Go, Rust, Python; servidores) e a referência do prelúdio gerada por `quark doc`.
> - De passagem: um nome de tipo dentro de uma lambda de uma função genérica (`T`), e o arquivo certo num pânico em
>   código de outro arquivo (um `assert` da std dizia o caminho do programa).

- **std:** TLS por `quark bind` de biblioteca do sistema (Schannel, Security.framework, OpenSSL) atrás de uma
  interface única; `std.json` tipado (de e para records, gerado em tempo de compilação por B2); CSV; diretórios
  recursivos; subprocessos com entrada e saída em fluxo; relógio com fusos nomeados.
- **Pacotes:** registro ou convenção de nomes, versões semânticas, `quark get` com verificação de hash.
- **LSP:** renomear, ir para implementações, efeitos e tipos ao passar o mouse, diagnósticos incrementais (com B10).
- **Testes:** `quark test` com filtro, cobertura, testes de propriedade na `std.testing`.
- **Documentação:** tutorial do zero, guia "Quark para quem vem de Go/Rust/Python", guia de servidores, e a
  referência gerada do prelúdio (B1) em vez de escrita à mão.

## 7. Ordem e dependências

```
A (segurança) ──► B4 (tirar contado) ──► B1 (prelúdio) ──► B2 (constantes por execução)
                                   │                  └──► B5 (runtime inlinável)
                                   └──► B3 (MIR) ──► B7 (memória) 
C (linguagem) em paralelo com B1; B6 (layout) depois de B3; B9, B10, D, E independentes
```

| Marco | Conteúdo | Critério |
| --- | --- | --- |
| **0.9** | Fase A, B4 | git limpo, release publicado, CI em 3 sistemas, teste diferencial rodando |
| **0.10** | B1, Fase C, B9 | um built-in em um lugar só; decisões de linguagem tomadas e implementadas |
| **0.11** | B3 (pelo menos o desaçucaramento), B2, B5 | `lower.qk` e `consteval.qk` com menos da metade do tamanho; `words` abaixo de 1,1× o C |
| **0.12** | B6, B7, B10, D | `records` abaixo de 1,5× o C; ARM64 em paridade; rebuild abaixo de 0,5 s |
| **1.0** | Fase E, congelamento | STABILITY.md vale; nenhuma mudança de layout ou de sintaxe pendente |

## 8. O que eu não faria

- **Voltar à contagem de referências** como modelo geral: a medição já respondeu.
- **Um otimizador por e-graphs ou um tier de passada única:** nada mediu a necessidade.
- **Agendador multi-thread com memória compartilhada:** troca a garantia mais valiosa do modelo (tarefas não
  compartilham nada) por uma classe inteira de bugs.
- **Macros ou metaprogramação textual:** B2 (execução em tempo de compilação) cobre os casos reais.
- **Escrever TLS em Quark:** criptografia própria é um risco de segurança que nenhuma regra de "sem dependências"
  justifica.

## 9. Riscos do plano

| Risco | Mitigação |
| --- | --- |
| B1, B2 e B3 juntos paralisam o projeto | uma de cada vez, cada uma com o caminho antigo vivo até a paridade |
| Regressão de desempenho escondida | `tools/bench.sh` na CI, com limite de variação por benchmark |
| Bootstrap quebrado no meio de uma refatoração | cada passo compila com o release anterior; tag antes de cada marco |
| Plataformas não testadas divergem | A2 primeiro: nada estrutural antes de a CI rodar em três sistemas |
| Escopo cresce sem fim | o 1.0 é definido pelo que congela, não por features: o que não é contrato pode entrar depois |
