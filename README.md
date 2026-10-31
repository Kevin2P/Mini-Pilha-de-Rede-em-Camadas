<div align="center">

# Mini-Pilha de Rede em Camadas

**Pilha de quatro camadas em C sobre soquetes UDP, com enlace simulado**

</div>

**Autor:** Kevin Miyahara Omori

---

## Visão geral

Cada instância do programa `ministack` é um nó com endereço lógico de 16 bits. Os nós se comunicam por UDP real, que faz o papel do meio físico; a camada de enlace simula perda, atraso, jitter e corrupção dos frames.

```
 Nó 1                                    Nó 2
 Aplicação  (chat, benchmark)            Aplicação
 Transporte (portas, ACK, retransmissão) Transporte
 Rede       (endereço, rota, TTL)        Rede
 Enlace     (CRC-16, simulação)          Enlace    frames em datagramas UDP
```

A aplicação oferece um chat com sessão (porta lógica 1) e um benchmark (porta 2). O transporte tem ACK cumulativo, janela deslizante (go-back-N), retransmissão por temporizador e um modo não confiável. Cada camada só inclui o cabeçalho da camada logo abaixo; a comunicação para cima usa callbacks: `src/node.c` (o único módulo que conhece todas as camadas) liga o enlace à rede e a rede ao transporte, e a aplicação registra os seus no transporte com `tp_bind`. Os testes verificam essa regra automaticamente.

## Compilar

Requisitos: Linux, `gcc`, `make` e Python 3.

```bash
make
```

Gera `bin/ministack` (o nó) e `bin/test_codec` (testes unitários).

## Como testar

**1. Testes automáticos** (cerca de 1 minuto; usam portas UDP a partir de 30000): 13 verificações de codec e robustez e 85 de integração entre processos, incluindo a conformidade independente com os formatos deste README, as relações medidas no relatório e os caminhos de erro da linha de comando.

```bash
make test
make sanitize      # os mesmos testes com AddressSanitizer e UBSan
make fuzz          # fuzzing dos protocolos com CRC válido, sob AddressSanitizer e UBSan (cerca de 20 s)
```

Os passos seguintes usam as portas UDP 9001 a 9003: encerre cada nó com Ctrl+C (ou `/quit` no chat) antes de passar ao próximo.

**2. Chat entre dois nós, vendo o encapsulamento.** Em dois terminais (o `-v` do primeiro mostra, em cada camada, o cabeçalho que é acrescentado ao enviar e removido ao receber):

```bash
./bin/ministack -a 1 -p 9001 -n 2=127.0.0.1:9002 -d 2 -U alice -v
./bin/ministack -a 2 -p 9002 -n 1=127.0.0.1:9001 -U bob
```

Digite uma linha em qualquer terminal para enviá-la; `/quit` encerra a sessão. Para ver a mesma demonstração de forma automática: `python3 scripts/demo_chat.py`. Para um meio ruim (30% de perda e 20 ms de atraso), acrescente `-l 0.3 -D 20 -t 100` nos dois nós: as mensagens continuam chegando, mais devagar, e no terminal do `alice` as linhas `TRP RTO` mostram o temporizador expirando e as retransmissões.

**3. Benchmark confiável com 10% de perda.** O receptor imprime as estatísticas ao receber Ctrl+C:

```bash
./bin/ministack -a 2 -p 9002 -n 1=127.0.0.1:9001 -m sink -l 0.1
./bin/ministack -a 1 -p 9001 -n 2=127.0.0.1:9002 -m send -d 2 -N 100 -s 100 -l 0.1 -t 60
```

O emissor imprime `STAT tp.retransmissions=...` e o receptor, `STAT app.bench_unique=100` e `STAT app.bench_in_order=100`: tudo chega, em ordem, apesar da perda.

**4. Comparação com o transporte não confiável** (`-u`, 30% de perda; sem ACK nem retransmissão, `app.bench_unique` fica abaixo de 100 e `tp.unrel_gaps` conta as lacunas):

```bash
./bin/ministack -a 2 -p 9002 -n 1=127.0.0.1:9001 -m sink -l 0.3
./bin/ministack -a 1 -p 9001 -n 2=127.0.0.1:9002 -m send -d 2 -N 100 -s 100 -u -l 0.3
```

**5. Detecção de erros** (20% dos frames com bits invertidos). Com o CRC ligado, `link.rx_bad_checksum` conta os frames rejeitados e nenhum dado corrompido chega à aplicação. Para desligar a verificação, acrescente `-x` nos dois nós e observe `app.bench_bad_payload` no receptor:

```bash
./bin/ministack -a 2 -p 9002 -n 1=127.0.0.1:9001 -m sink -e 0.2
./bin/ministack -a 1 -p 9001 -n 2=127.0.0.1:9002 -m send -d 2 -N 100 -e 0.2 -t 60
```

**6. Três nós, com encaminhamento** (1 envia para 3 através de 2). O nó 2 imprime `net.forwarded` ao receber Ctrl+C:

```bash
./bin/ministack -a 3 -p 9003 -n 2=127.0.0.1:9002 -r 1=2 -m sink
./bin/ministack -a 2 -p 9002 -n 1=127.0.0.1:9001 -n 3=127.0.0.1:9003 -m sink -v
./bin/ministack -a 1 -p 9001 -n 2=127.0.0.1:9002 -r 3=2 -m send -d 3 -N 20
```

Com `-T 1` no nó 1, o TTL se esgota no nó 2 e `net.ttl_expired` aumenta; o emissor desiste após as retransmissões e sai com código 3.

**7. Experimentos e relatório.** Precisam de matplotlib e TeX Live; no Ubuntu: `sudo apt install python3-matplotlib texlive-latex-extra texlive-pictures texlive-lang-portuguese texlive-fonts-recommended`. Os comandos gravam CSVs em `results/` e depois tabelas e figuras em `report/generated/`:

```bash
python3 scripts/run_experiments.py            # todos (cerca de 4 minutos); ou: loss delay corruption overhead reliability window window_sweep
python3 scripts/make_assets.py
cd report && pdflatex report.tex && pdflatex report.tex
```

Os CSVs já estão em `results/`: para refazer só as tabelas, as figuras e o relatório, pule o primeiro comando. Ao refazer os experimentos, as contagens (retransmissões, entregas, bytes) se reproduzem exatamente e os tempos variam alguns por cento (até 5,4% nos casos medidos); por isso o relatório trata os tempos como aproximados e as relações entre eles como o resultado.

## Opções do programa

Todas as opções têm uma letra só. As estatísticas saem como linhas `STAT camada.contador=valor`. As mensagens do programa são ASCII, sem acentos, para não depender da codificação do terminal.

| Opção | Significado |
|---|---|
| `-a N` `-p N` | endereço lógico do nó e porta UDP local (obrigatórios) |
| `-n A=IP:P` | vizinho direto (repetível) |
| `-r D=A` | rota estática: pacotes para `D` saem pelo vizinho `A`, que precisa ter sido declarado com `-n` (repetível) |
| `-m MODO` | `chat` (padrão), `sink` ou `send` |
| `-d A` | nó de destino (chat: abre a sessão; send: destino do benchmark) |
| `-U NOME` `-c TEXTO` | nome no chat; texto enviado assim que a sessão abre |
| `-N N` `-s N` `-i MS` `-u` | benchmark: mensagens (padrão 100, até 1048576), tamanho em bytes (mínimo 5, padrão 100), intervalo, modo não confiável |
| `-l P` `-e P` | probabilidade de perda e de corrupção de frames (0 a 1) |
| `-D MS` `-j MS` | atraso do enlace e jitter |
| `-x` | desliga a verificação do CRC-16 |
| `-S N` | semente do gerador pseudoaleatório (inteiro sem sinal) |
| `-w N` `-t MS` `-R N` | janela (de 1 a 1024, padrão 4), temporizador de retransmissão (200) e limite de retransmissões (10) |
| `-T N` | TTL dos pacotes (padrão 16) |
| `-W S` `-E MS` | tempo máximo do modo send, em segundos (padrão 120); modo sink encerra após MS sem tráfego |
| `-P` `-v` | imprime estatísticas ao sair; mostra o encapsulamento em cada camada |

Códigos de saída: 0 sucesso, 1 erro de uso (qualquer valor fora da faixa é rejeitado), 2 tempo máximo do modo send esgotado, 3 falha de entrega (limite de retransmissões atingido). No modo chat, o fim da entrada padrão encerra o nó, como no `nc`.

## Formatos dos cabeçalhos

Campos multi-byte em ordem de rede (big-endian).

| Camada | Bytes | Campos |
|---|---|---|
| Enlace | 4 | tamanho (2), CRC-16 (2): polinômio 0x1021, valor inicial 0xFFFF |
| Rede | 9 | versão (1), TTL (1), origem (2), destino (2), protocolo (1), tamanho (2) |
| Transporte | 13 | porta de origem (2), porta de destino (2), sequência (4), confirmação (4), flags (1): `DATA` 0x01, `ACK` 0x02, `REL` 0x04 |
| Chat | 1 + texto | tipo (1): `HELLO` 1, `HELLO_ACK` 2, `TEXT` 3, `BYE` 4; texto de até 200 bytes |
| Benchmark | 5 + padrão | tipo (1) igual a 10, identificador (4), bytes de padrão |

## Resultados

Medidos pelos scripts, com 3 sementes por configuração (os parâmetros de cada experimento estão na Tabela 4 do relatório):

| Experimento | Resultado |
|---|---|
| Sem perda | 100 mensagens em 260 ms, 0 retransmissões |
| 30% de perda, confiável | 100% entregues em ordem, em 3413 ms; 65,9% dos segmentos são retransmissões |
| 30% de perda, não confiável | 67,7% entregues |
| Atraso de 50 ms por salto, janela 1, 4 e 16 | 5023, 1306 e 402 ms (modelo: ⌈N/janela⌉ × RTT) |
| Janela de 8 a 64, com 5% de perda e 1 ms de atraso | o tempo se estabiliza em torno de 1,4 s, enquanto as retransmissões sobem de 28,9% para 75,5% |
| Overhead | (S + 52)/S no modo confiável e (S + 26)/S no não confiável, igual ao medido |
| 20% de corrupção | com CRC, 100% íntegras; sem CRC, 84,0% íntegras |

## Estrutura do projeto

```
ministack/
├── include/            contratos de cada camada (common, link, network, transport, application)
├── src/
│   ├── application.c   chat com sessão e benchmark
│   ├── transport.c     portas, ACK, janela, retransmissão
│   ├── network.c       endereços, rotas, TTL, encaminhamento
│   ├── link.c          frame, CRC-16, simulação do meio, soquete UDP
│   ├── node.c          opções, ligação entre camadas, laço de eventos
│   └── common.c        relógio, inteiros em ordem de rede, trace
├── tests/test_codec.c  testes de codec e de robustez (fuzz)
├── scripts/            testes (run_tests, conformance, fuzz_protocol, harness), experimentos (run_experiments, make_assets) e demo_chat
├── results/            CSVs dos experimentos
├── report/             relatório técnico (LaTeX e PDF)
└── Makefile
```
