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

> **Próximos passos:**
> - As decisões que ainda dependem do estado da emissão: o self por endereço, a área da função e a coleta por turno de laço, que lê o IR emitido.
> - O último uso estendido a campos (`self.items` no fim) e a ramos de `match` e `if`.
> - Reutilização no lugar (`xs = xs.map(...)` sobre um var único).

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

### B8. O grafo sem JSON por dentro

O código do grafo monta e lê JSON e converte para QGN na borda. Trabalhar sobre os valores de `std.graph`
simplifica `graph.qk`/`collab.qk` (4.000 linhas) e deixa programas Quark manipularem grafos com tipos.

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
- **Cancelamento e prazos:** `timeout(seconds, f)`, fechar um canal acorda quem espera.
- **Concorrência estruturada:** um escopo que só termina quando as tarefas que abriu terminam, e propaga o pânico
  com a origem (`go` solto fica para servidores).
- **Decisão a tomar e escrever:** uma thread por agendador (hoje) ou um agendador por núcleo com roubo de trabalho.
  Recomendo ficar em uma thread no 1.0 e oferecer `serve(..., workers: n)` como processos ou threads isoladas que
  compartilham o socket: mantém o modelo "tarefas não compartilham memória" e escala em núcleos.

### B10. Compilação incremental de ponta a ponta

Cache por módulo e por conteúdo para parse, checagem e MIR (o de otimização já existe), lowering em paralelo por
função, threads que não recarregam o código (o custo de 10 ms). Meta: rebuild do compilador abaixo de 0,5 s e
`quark run` de um programa pequeno abaixo de 30 ms.

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
- **Informação de depuração padrão:** DWARF e PDB/CodeView, para depuradores e perfiladores externos.
- **wasm32:** rodar a suíte num runtime wasm; `std` sem sistema bem delimitada.
- **libquark no macOS** e a API C revisada uma última vez antes de congelar.
- **Executáveis menores:** só o que o programa usa do runtime (hoje vai inteiro).

> **Medido em 20/11/2026.** Um "hello" tem 842 KB, dos quais 838 KB são o runtime. A imagem do runtime é guardada em cache uma vez por sistema, e o programa vai junto, carregado pelo runtime ao começar. A tabela de funções do runtime é preenchida com todas elas. Para deixar de fora as não usadas, é preciso compilar o runtime por programa, com alcançabilidade a partir das chamadas do programa e uma tabela parcial, e guardar o cache pelo conjunto de funções usadas.

## 6. Fase E: biblioteca e ferramentas

> **Feito em 19/11/2026:** `std.csv`, `walkFiles` no `std.files`, `quark test --filter`, e o tutorial
> ([TUTORIAL.md](TUTORIAL.md), dez passos, seus exemplos verificados pela suíte). Antes disso já tinham entrado
> `std.encoding`, `std.hash`, `std.args`, `std.files`, `std.net` e `std.http`. Faltam: TLS, JSON tipado,
> subprocessos em fluxo, fusos nomeados, registro de pacotes, LSP (renomear, efeitos ao passar o mouse).

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
