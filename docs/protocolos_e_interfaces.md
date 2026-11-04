# Protocolos, módulos de comunicação e interfaces

## Camadas da comunicação

| Trecho | Tecnologia | Detalhe |
|---|---|---|
| Sensores e placa | I2C e SMBus | Barramentos separados, ver [hardware.md](hardware.md) |
| Placa e roteador | Wi-Fi 802.11 b/g/n, 2,4 GHz | Rádio integrado ao ESP32-S3, modo estação, sem economia de energia (`WiFi.setSleep(false)`) |
| Placa e broker | MQTT 3.1.1 sobre TCP | `broker.hivemq.com`, porta 1883, sem criptografia |
| Painel e broker | MQTT sobre WebSocket com TLS | `wss://broker.hivemq.com:8884/mqtt` |
| Monitor serial | UART pelo conversor CP2102, 115200 baud | Diagnóstico e calibração, sem papel na comunicação com a internet |

Não se usa Node-RED, comunicação serial com o computador nem Firmata na comunicação com a internet, porque a placa conecta direto ao broker.

## Clientes MQTT

| Cliente | Identificador | Parâmetros |
|---|---|---|
| Placa | `oic-monitor-` seguido do endereço MAC | Keep alive de 15 s; última vontade `offline`, retida, QoS 1, no tópico `status` |
| Painel | `oic-painel-` seguido de um valor aleatório | Reconexão a cada 3 s; sessão limpa |

## Tópicos

Prefixo de todos os tópicos: `mackenzie/oic/monitor-sinais-vitais/`.

### `leituras`

Publicado pela placa em cada ciclo, com QoS 0.

```json
{"seq":327,"dedo":true,"bpm":74,"spo2":99,"temp":36.42,"pele":34.48,"estado":"normal"}
```

| Campo | Tipo | Significado |
|---|---|---|
| `seq` | inteiro | Contador de leituras desde que a placa ligou |
| `dedo` | booleano | Dedo detectado no MAX30102 |
| `bpm` | inteiro ou nulo | Mediana da frequência cardíaca |
| `spo2` | inteiro ou nulo | Mediana da saturação, em % |
| `temp` | número ou nulo | Temperatura com a correção, em °C |
| `pele` | número ou nulo | Temperatura lida pelo sensor, em °C |
| `estado` | texto | `sem_leitura`, `normal`, `aviso` ou `alarme` |

### `alerta`

Publicado pela placa com QoS 1, sempre que o estado muda.

```json
{"estado":"alarme","fc":"normal","spo2":"alarme","temp":"normal","bpm":74,"spo2_valor":90,"temp_valor":36.42}
```

### `status`

Texto `online` ou `offline`, com QoS 1 e retenção. Quem assina recebe o último valor logo ao se conectar. O broker publica `offline` pela placa se a conexão cair sem aviso.

### `comando`

Publicado pelo painel com QoS 1, no formato `id;acao`.

| Ação | Efeito |
|---|---|
| `verde`, `amarelo`, `vermelho`, `azul` | Acende o LED nessa cor por 5 s |
| `apagado` | Apaga o LED por 5 s |
| `auto` | Devolve o LED ao controle pelas leituras |
| `medir` | Inicia 4 rodadas de medição dos sensores |

### `confirmacao`

Publicado pela placa com QoS 1 depois de executar um comando. O campo `led_us` é o tempo gasto na escrita do LED, em microssegundos.

```json
{"id":"15-1793754491720","acao":"amarelo","valido":true,"led_us":1399}
```

### `medicao/max30102` e `medicao/mlx90614`

Publicados e assinados pela própria placa com QoS 0, para medir a ida e a volta até o broker.

```json
{"n":1,"dedo":true,"bpm":74,"spo2":99}
{"n":1,"pele":34.48}
```

### `medicao_resultado`

Publicado pela placa com QoS 0.

```json
{"sensor":"max30102","n":1,"dedo":true,"leitura_ate_envio_ms":9.83,"ida_volta_ms":673.55,"tempo_ms":346.60}
```

Os exemplos de `leituras`, `alerta`, `medicao` e `medicao_resultado` mostram o formato. Os valores medidos estão em [`dados/`](../dados).

## Interface do usuário

| Interface | Entrada | Saída |
|---|---|---|
| Display OLED | Nenhuma | Frequência, saturação, temperatura e indicador `MQTT` ou `----` |
| LED RGB | Comando do painel | Cor por estado |
| Painel web | Botões de LED, de medição e de CSV | Cartões de leitura, aviso, som, tabela e registro de mensagens |
| Monitor serial | Comandos de calibração | Registro de cada ciclo |

## Por que cada QoS

- Leituras e eco usam QoS 0, porque uma medição perdida logo é substituída pela seguinte.
- Alerta, comando, confirmação e status usam QoS 1, para que o broker confirme a entrega.
- `medicao_resultado` usa QoS 0, porque com QoS 1 a espera pela confirmação atrasava as rodadas seguintes da medição.

## Segurança

O broker é público e não autentica os clientes, e qualquer um que conheça o tópico pode ler ou publicar. Em um uso real seriam necessários um broker privado, autenticação, TLS também na placa e controle de acesso por tópico.
