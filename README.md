# SilverLink — áudio em rede PC ↔ PC (estilo Dante)

SilverLink transforma cada PC Windows numa "placa de som de rede" com **32 entradas × 32 saídas**
via ASIO. Os PCs trocam áudio pela rede Ethernet (multicast UDP, PCM 24 bits), sincronizados
por um relógio de rede, e as rotas são feitas num painel de matriz, como no Dante Controller.

| Componente | Arquivo | Função |
|---|---|---|
| **Engine** | `SilverLink.exe` | Roda na bandeja. Rede, relógio, transmissão/recepção, rotas. |
| **Driver ASIO** | `SilverLink_ASIO.dll` | Aparece no REAPER como "SilverLink". 32 in / 32 out, float 32 bits. |
| **Controller** | `SilverLink_Controller.exe` | Matriz de roteamento, dispositivos, relógio, configurações. |

## Compilar pelo GitHub

1. Suba esta pasta para um repositório no GitHub.
2. O workflow `.github/workflows/build.yml` roda sozinho (aba **Actions**).
3. Baixe o artefato **SilverLink-Setup** (instalador) ou **SilverLink-binarios** (exe/dll soltos).

Dependências baixadas automaticamente: Dear ImGui (MIT) e o ASIO SDK da Steinberg (licença GPLv3).

## Instalar

Rode `SilverLink_Setup_1.0.0.exe` **em cada PC**. O instalador:
- registra o driver ASIO,
- cria as regras de firewall (UDP),
- coloca o Engine para iniciar com o Windows (opcional) e já o inicia.

## Usar com o REAPER

1. Confira o ícone do SilverLink na bandeja. No menu dele, em **Interface de rede**, escolha a placa **cabeada**.
2. REAPER → *Preferences → Audio → Device* → **ASIO** → driver **SilverLink**, 44.1 kHz, buffer 256 (ou o que preferir).
3. Abra o **SilverLink Controller**. Na matriz, colunas = transmissores (saídas ASIO de cada PC),
   linhas = receptores (entradas ASIO de cada PC). Clique numa célula para rotear e clique de novo para desfazer.
   - Clique no nome do dispositivo para recolher/expandir.
   - Botão direito numa célula de resumo abre **Rotear 1:1** (canal N → canal N).
4. No REAPER, as saídas de hardware 1–32 da faixa vão para a rede, e as entradas 1–32 recebem da rede.

Cores das rotas: **verde** = recebendo; **amarelo** = rota feita, mas a origem não está transmitindo
(ASIO parado lá); **laranja** = taxas de amostragem diferentes.

> O REAPER usa um único driver ASIO por vez. Com o SilverLink selecionado, a sua interface de áudio
> física não fica ativa no mesmo projeto. Use o SilverLink no PC que só envia/recebe pela rede, ou
> escute pelo PC que tem a interface física.

## Teste rápido sem REAPER

Controller → **Configurar** → marque **Tom de teste** no PC de origem (1 kHz nos canais ímpares,
500 Hz nos pares, −20 dBFS). Roteie para o outro PC e veja os medidores no Controller.

## Latência

- **Latência de rede (recepção)**: 1, 2, 4 (padrão), 6, 10 ou 20 ms, definida por PC receptor.
- Latência total de saída do REAPER A → entrada do REAPER B = **2 × buffer ASIO + latência de rede**
  (ex.: buffer 256 a 44.1 kHz e 4 ms → ≈ 15,6 ms).
- Se a coluna **Atrasados** (aba Dispositivos) subir, aumente a latência de rede do receptor.

## Relógio

Um PC é eleito **líder** automaticamente (ou marque **Líder preferido**). Os outros seguem o
relógio dele com um PTP simplificado (Sync + Delay Req/Resp, servo PI), então todos os PCs
processam blocos alinhados na mesma linha do tempo, sem deriva. Estado e offset ficam na aba **Relógio**.

## Rede — recomendações

- Cabo e switch **gigabit**. 32 canais a 44.1 kHz ≈ 34 Mbit/s por PC transmissor.
- Switch gerenciável: **IGMP Snooping ligado** e um **IGMP Querier** ativo na VLAN; senão o
  multicast vira broadcast. Se possível, desligue **EEE / Green Ethernet** nas portas de áudio.
- Placa de rede no Windows: desligue *Energy Efficient Ethernet* e, se houver estalos, teste desligar
  *Interrupt Moderation* e *Flow Control*. Plano de energia: **Alto desempenho**.
- Portas: UDP 7700 (descoberta), 7701 (controle), 7702 (relógio), 7710 (áudio).
  Grupos: 239.255.77.77, 239.255.77.78 e 239.77.x.y (um por PC).

## Arquitetura

```
REAPER ──ASIO──► SilverLink_ASIO.dll ◄──memória compartilhada──► SilverLink.exe ◄──UDP multicast──► outros PCs
                                                                       ▲
                                                SilverLink_Controller ──┘ (descoberta + comandos)
```

- Linha do tempo comum: amostra = tempo de rede (ns) × taxa / 10⁹.
- O Engine sinaliza o driver a cada bloco (múltiplo do buffer) pelo relógio de rede.
- Saída do bloco S recebe o carimbo S+B. O receptor toca em carimbo + latência.
- Pacotes: cabeçalho de 36 bytes + 14 quadros × 32 canais × 24 bits (≈1,4 kB).
- Configuração do Engine: `%APPDATA%\SilverLink\engine.cfg`.

## Limites desta versão

- Só PC ↔ PC SilverLink (não conversa com equipamentos Dante; para isso seria preciso AES67).
- Apenas ASIO (sem driver WDM para o áudio geral do Windows).
- Rede sem autenticação: use numa rede de áudio dedicada/confiável.

## Licença

GPLv3 (veja `LICENSE`), por usar o ASIO SDK sob a opção GPLv3.
ASIO é marca registrada da Steinberg Media Technologies GmbH. "Dante" é marca da Audinate;
o SilverLink não tem relação com a Audinate nem é compatível com o protocolo Dante.
