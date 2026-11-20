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

### B7. Memória: copiar menos na coleta de laço

- **Pré-promoção:** o que é guardado num container de fora do laço é alocado direto na região de fora.
- **Análise de escape** (sobre a MIR de B3): o que não sai da função vai para a pilha ou registradores.
- **Reutilização no lugar:** atualizar campo ou elemento de um valor único nunca aloca.
- **Barreira de escrita filtrada no código gerado** (`remember` ainda custa 5 a 7% em `json`).

Meta: `records` de 2,2× para menos de 1,5× o C.

### B8. O grafo sem JSON por dentro

O código do grafo monta e lê JSON e converte para QGN na borda. Trabalhar sobre os valores de `std.graph`
simplifica `graph.qk`/`collab.qk` (4.000 linhas) e deixa programas Quark manipularem grafos com tipos.

### B9. Concorrência: do protótipo ao modelo

- **`Channel<T>` tipado** (o runtime já copia qualquer valor entre arenas).
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
- **Sem libc onde der:** Ryū para escrever `Float`, parser próprio para ler, memória direto do sistema. No Windows
  e no Linux o executável passa a depender só do kernel; no macOS a `libSystem` é obrigatória.
- **Informação de depuração padrão:** DWARF e PDB/CodeView, para depuradores e perfiladores externos.
- **wasm32:** rodar a suíte num runtime wasm; `std` sem sistema bem delimitada.
- **libquark no macOS** e a API C revisada uma última vez antes de congelar.
- **Executáveis menores:** só o que o programa usa do runtime (hoje vai inteiro).

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
