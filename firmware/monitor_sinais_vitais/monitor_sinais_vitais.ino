#include <Wire.h>
#include <Preferences.h>
#include <WiFi.h>
#include <MQTT.h>
#include "MAX30105.h"
#include "spo2_algorithm.h"
#include <Adafruit_MLX90614.h>
#include <U8g2lib.h>
#include "segredos.h"

MAX30105 max30102;
Adafruit_MLX90614 mlx;
U8G2_SSD1306_128X64_NONAME_F_SW_I2C tela(U8G2_R0, 16, 15, U8X8_PIN_NONE);
Preferences prefs;
WiFiClient redeTcp;
MQTTClient mqtt(768);

const char* BROKER = "broker.hivemq.com";
const int PORTA_MQTT = 1883;
#define BASE "mackenzie/oic/monitor-sinais-vitais"
const char* T_LEITURAS = BASE "/leituras";
const char* T_ALERTA = BASE "/alerta";
const char* T_COMANDO = BASE "/comando";
const char* T_CONFIRMACAO = BASE "/confirmacao";
const char* T_STATUS = BASE "/status";
const char* T_MED_MAX = BASE "/medicao/max30102";
const char* T_MED_MLX = BASE "/medicao/mlx90614";
const char* T_MED_RES = BASE "/medicao_resultado";
const unsigned long LED_MANUAL_MS = 5000;
const int RODADAS_MEDICAO = 4;

const byte BRILHO_LED_SENSOR = 30;
const int LED = 48;
const int FC_NORMAL_MIN = 51, FC_NORMAL_MAX = 90, FC_ALARME_MIN = 40, FC_ALARME_MAX = 131;
const int SPO2_NORMAL = 96, SPO2_ALARME = 91;
const float T_NORMAL_MIN = 36.1, T_NORMAL_MAX = 38.0, T_ALARME_MIN = 35.0, T_ALARME_MAX = 39.1;
const unsigned long ESTABILIZACAO = 5000;
const unsigned long ESTABILIZACAO_FC = 12000;
const int CONFIRMACOES = 3;
const int PARES_CALIBRACAO = 5;
const float CORRECAO_INICIAL = 1.65;
const float VARIACAO_MAX_LEITURA = 0.3;

struct Historico {
  float v[5];
  int n = 0, p = 0;
  void add(float x) { v[p] = x; p = (p + 1) % 5; if (n < 5) n++; }
  void limpa() { n = 0; p = 0; }
  float mediana() {
    float a[5];
    for (int i = 0; i < n; i++) a[i] = v[i];
    for (int i = 1; i < n; i++) {
      float x = a[i];
      int j = i - 1;
      while (j >= 0 && a[j] > x) { a[j + 1] = a[j]; j--; }
      a[j + 1] = x;
    }
    return (n % 2) ? a[n / 2] : (a[n / 2 - 1] + a[n / 2]) / 2;
  }
  float amplitude() {
    float mn = v[0], mx = v[0];
    for (int i = 1; i < n; i++) { mn = min(mn, v[i]); mx = max(mx, v[i]); }
    return mx - mn;
  }
};

Historico hBpm, hSpo2, hPele;
const int TAM = 100;
uint32_t ir[TAM], red[TAM];
int n = 0, novas = 0;
int32_t spo2, bpmMaxim;
int8_t spo2Valido, bpmMaximValido;
const int AMOSTRAS_SEG = 25;
const int TAM_FC = 200;
const float CORRELACAO_MIN = 0.5;
float irFc[TAM_FC], sinal[TAM_FC], aux[TAM_FC];
int nFc = 0, pFc = 0;
float bpm = 0, qualidadeFc = 0;
bool bpmValido = false;

bool calculaFc() {
  if (nFc < TAM_FC) return false;
  for (int i = 0; i < TAM_FC; i++) sinal[i] = irFc[(pFc + i) % TAM_FC];
  for (int i = 0; i < TAM_FC; i++) {
    int a = max(0, i - 8), b = min(TAM_FC - 1, i + 8);
    float s = 0;
    for (int j = a; j <= b; j++) s += sinal[j];
    aux[i] = sinal[i] - s / (b - a + 1);
  }
  float media = 0;
  for (int i = 0; i < TAM_FC; i++) {
    int a = max(0, i - 2), b = min(TAM_FC - 1, i + 2);
    float s = 0;
    for (int j = a; j <= b; j++) s += aux[j];
    sinal[i] = s / (b - a + 1);
    media += sinal[i];
  }
  media /= TAM_FC;
  for (int i = 0; i < TAM_FC; i++) sinal[i] -= media;

  const int kMin = AMOSTRAS_SEG * 60 / 180, kMax = AMOSTRAS_SEG * 60 / 40;
  float r[kMax + 2];
  for (int k = kMin - 1; k <= kMax + 1; k++) {
    float suv = 0, suu = 0, svv = 0;
    for (int i = 0; i + k < TAM_FC; i++) {
      suv += sinal[i] * sinal[i + k];
      suu += sinal[i] * sinal[i];
      svv += sinal[i + k] * sinal[i + k];
    }
    r[k] = (suu > 0 && svv > 0) ? suv / sqrt(suu * svv) : 0;
  }
  int kMelhor = kMin;
  for (int k = kMin; k <= kMax; k++) if (r[k] > r[kMelhor]) kMelhor = k;
  if (r[kMelhor] < CORRELACAO_MIN) { qualidadeFc = r[kMelhor]; return false; }
  for (int k = kMin; k < kMelhor; k++) {
    if (r[k] >= 0.7 * r[kMelhor] && r[k] >= r[k - 1] && r[k] >= r[k + 1]) { kMelhor = k; break; }
  }
  float den = r[kMelhor - 1] - 2 * r[kMelhor] + r[kMelhor + 1];
  float desloc = (den != 0) ? 0.5 * (r[kMelhor - 1] - r[kMelhor + 1]) / den : 0;
  if (desloc > 0.5 || desloc < -0.5) desloc = 0;
  bpm = 60.0 * AMOSTRAS_SEG / (kMelhor + desloc);
  qualidadeFc = r[kMelhor];
  return true;
}
bool maxOk, mlxOk, dedoAtual = false;
unsigned long ultimaTela = 0, ultimoLed = 0, dedoDesde = 0;
unsigned long ultimoBpmOk = 0, ultimaSpo2Ok = 0;
const unsigned long VALIDADE_LEITURA = 10000;
const unsigned long LIMITE_SEM_AMOSTRA = 3000;
unsigned long ultimaAmostra = 0;
int estado = 0, candidatoAnterior = -1, repeticoes = 0, estadoPublicado = -1;
int classeFc = 0, classeSpo2 = 0, classeTemp = 0;

bool tempCalibrada = false;
float correcaoTemp = 0.0;
float diferencas[PARES_CALIBRACAO];
int nPares = 0;
String comando = "";

struct Saida {
  char topico[72];
  char corpo[240];
  bool retida;
  uint8_t qos;
  int8_t sensor;
  int8_t rodada;
};
struct Medicao {
  volatile uint32_t tDeteccao[RODADAS_MEDICAO], tPublicacao[RODADAS_MEDICAO];
  volatile bool dedo[RODADAS_MEDICAO];
};
QueueHandle_t filaSaida;
SemaphoreHandle_t travaLed;
Medicao medicoes[2];
volatile int rodadasMedicao = 0, numeroRodada = 0;
volatile bool mqttConectado = false;
volatile unsigned long ledManualAte = 0;
uint32_t seqLeitura = 0, tDeteccaoMax = 0, tDeteccaoMlx = 0;

void escreveLed(int r, int g, int b) {
  xSemaphoreTake(travaLed, portMAX_DELAY);
  rgbLedWrite(LED, r, g, b);
  xSemaphoreGive(travaLed);
}

void enfileira(const char* topico, const char* corpo, bool retida, int qos, int sensor = -1, int rodada = 0) {
  Saida s;
  strncpy(s.topico, topico, sizeof(s.topico) - 1);
  s.topico[sizeof(s.topico) - 1] = '\0';
  strncpy(s.corpo, corpo, sizeof(s.corpo) - 1);
  s.corpo[sizeof(s.corpo) - 1] = '\0';
  s.retida = retida;
  s.qos = qos;
  s.sensor = sensor;
  s.rodada = rodada;
  xQueueSend(filaSaida, &s, 0);
}

void aoReceber(String& topico, String& corpo) {
  uint32_t agora = micros();
  if (topico == T_MED_MAX || topico == T_MED_MLX) {
    int i = topico == T_MED_MAX ? 0 : 1;
    int numero = corpo.substring(corpo.indexOf(':') + 1).toInt();
    if (numero < 1 || numero > RODADAS_MEDICAO) return;
    int r = numero - 1;
    float ateEnvio = (medicoes[i].tPublicacao[r] - medicoes[i].tDeteccao[r]) / 1000.0;
    float idaVolta = (agora - medicoes[i].tPublicacao[r]) / 1000.0;
    float tempo = ateEnvio + idaVolta / 2;
    char m[180];
    snprintf(m, sizeof(m), "{\"sensor\":\"%s\",\"n\":%d,\"dedo\":%s,\"leitura_ate_envio_ms\":%.2f,\"ida_volta_ms\":%.2f,\"tempo_ms\":%.2f}",
             i == 0 ? "max30102" : "mlx90614", numero, medicoes[i].dedo[r] ? "true" : "false", ateEnvio, idaVolta, tempo);
    enfileira(T_MED_RES, m, false, 0);
    Serial.printf("MEDICAO %s\n", m);
    return;
  }
  if (topico != T_COMANDO) return;
  int sep = corpo.indexOf(';');
  if (sep < 0) return;
  String id = corpo.substring(0, sep);
  String acao = corpo.substring(sep + 1);
  acao.trim();
  uint32_t ledUs = 0;
  bool valido = true;
  if (acao == "medir") {
    numeroRodada = 0;
    rodadasMedicao = RODADAS_MEDICAO;
  } else if (acao == "auto") {
    ledManualAte = 0;
  } else {
    int r = 0, g = 0, b = 0;
    if (acao == "verde") g = 40;
    else if (acao == "amarelo") { r = 40; g = 25; }
    else if (acao == "vermelho") r = 40;
    else if (acao == "azul") b = 40;
    else if (acao != "apagado") valido = false;
    if (valido) {
      ledManualAte = millis() + LED_MANUAL_MS;
      uint32_t t0 = micros();
      escreveLed(r, g, b);
      ledUs = micros() - t0;
    }
  }
  char m[120];
  snprintf(m, sizeof(m), "{\"id\":\"%s\",\"acao\":\"%s\",\"valido\":%s,\"led_us\":%lu}", id.c_str(), acao.c_str(), valido ? "true" : "false", (unsigned long)ledUs);
  enfileira(T_CONFIRMACAO, m, false, 1);
}

void conectaMqtt() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char idCliente[32];
  snprintf(idCliente, sizeof(idCliente), "oic-monitor-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  mqtt.setWill(T_STATUS, "offline", true, 1);
  if (mqtt.connect(idCliente)) {
    mqtt.subscribe(T_COMANDO, 1);
    mqtt.subscribe(T_MED_MAX, 0);
    mqtt.subscribe(T_MED_MLX, 0);
    mqtt.publish(T_STATUS, "online", true, 1);
    Serial.printf("MQTT conectado como %s\n", idCliente);
  } else {
    Serial.printf("MQTT sem conexao, erro %d\n", (int)mqtt.lastError());
  }
}

void tarefaRede(void*) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_NOME, WIFI_SENHA);
  mqtt.begin(BROKER, PORTA_MQTT, redeTcp);
  mqtt.setKeepAlive(15);
  mqtt.onMessage(aoReceber);
  unsigned long ultimaTentativa = 0;
  bool wifiAvisado = false;
  while (true) {
    if (WiFi.status() == WL_CONNECTED) {
      if (!wifiAvisado) {
        wifiAvisado = true;
        Serial.printf("Wi-Fi conectado, IP %s, sinal %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
      }
      if (!mqtt.connected() && millis() - ultimaTentativa > 5000) {
        ultimaTentativa = millis();
        conectaMqtt();
      }
      mqtt.loop();
    } else {
      wifiAvisado = false;
    }
    mqttConectado = mqtt.connected();
    Saida s;
    while (xQueueReceive(filaSaida, &s, 0) == pdTRUE) {
      if (!mqttConectado) continue;
      if (s.sensor >= 0) medicoes[s.sensor].tPublicacao[s.rodada] = micros();
      mqtt.publish(s.topico, s.corpo, s.retida, s.qos);
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

float temperatura() { return hPele.mediana() + correcaoTemp; }

int classifica(float x, float nMin, float nMax, float aMin, float aMax) {
  if (x <= aMin || x >= aMax) return 3;
  if (x < nMin || x > nMax) return 2;
  return 1;
}

int avalia(bool dedo) {
  classeFc = classeSpo2 = classeTemp = 0;
  if (dedo && hBpm.n >= 3) classeFc = classifica(hBpm.mediana(), FC_NORMAL_MIN, FC_NORMAL_MAX, FC_ALARME_MIN, FC_ALARME_MAX);
  if (dedo && hSpo2.n >= 3) classeSpo2 = classifica(hSpo2.mediana(), SPO2_NORMAL, 100, SPO2_ALARME, 1000);
  if (tempCalibrada && hPele.n >= 3) {
    float t = temperatura();
    if (t > 30 && t < 45) classeTemp = classifica(t, T_NORMAL_MIN, T_NORMAL_MAX, T_ALARME_MIN, T_ALARME_MAX);
  }
  return max(classeFc, max(classeSpo2, classeTemp));
}

void atualizaLed() {
  if (millis() - ultimoLed < 50) return;
  ultimoLed = millis();
  if (ledManualAte && (long)(ledManualAte - millis()) > 0) return;
  ledManualAte = 0;
  if (estado == 3) {
    bool aceso = (millis() / 300) % 2;
    escreveLed(aceso ? 40 : 0, 0, 0);
  } else if (estado == 2) {
    escreveLed(40, 25, 0);
  } else if (estado == 1) {
    escreveLed(0, 40, 0);
  } else {
    escreveLed(0, 0, 0);
  }
}

struct Quadro {
  char l[3][20];
  bool alarme[3];
  bool piscando;
  bool rede;
};
Quadro quadro;
portMUX_TYPE muxTela = portMUX_INITIALIZER_UNLOCKED;
unsigned long ultimoCiclo = 0, periodoCiclo = 0;

void tarefaTela(void*) {
  Quadro q;
  while (true) {
    portENTER_CRITICAL(&muxTela);
    q = quadro;
    portEXIT_CRITICAL(&muxTela);
    bool apaga = q.piscando && (millis() / 500) % 2;
    tela.clearBuffer();
    tela.setFont(u8g2_font_ncenB10_tr);
    for (int i = 0; i < 3; i++) {
      if (!(apaga && q.alarme[i])) tela.drawStr(0, 16 + 22 * i, q.l[i]);
    }
    tela.setFont(u8g2_font_5x7_tr);
    tela.drawStr(104, 7, q.rede ? "MQTT" : "----");
    tela.sendBuffer();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void mostra() {
  char l1[20], l2[20], l3[20];
  if (!dedoAtual) {
    snprintf(l1, sizeof(l1), "Coloque o dedo");
    snprintf(l2, sizeof(l2), " ");
  } else {
    if (hBpm.n >= 3) snprintf(l1, sizeof(l1), "BPM: %d", (int)round(hBpm.mediana()));
    else snprintf(l1, sizeof(l1), "BPM: medindo");
    if (hSpo2.n >= 3) snprintf(l2, sizeof(l2), "SpO2: %d%%", (int)round(hSpo2.mediana()));
    else snprintf(l2, sizeof(l2), "SpO2: medindo");
  }
  if (hPele.n == 0) snprintf(l3, sizeof(l3), "Temp: sem sensor");
  else if (tempCalibrada) snprintf(l3, sizeof(l3), "Temp: %.1f C", temperatura());
  else snprintf(l3, sizeof(l3), "Pele: %.1f C", hPele.mediana());

  portENTER_CRITICAL(&muxTela);
  strcpy(quadro.l[0], l1);
  strcpy(quadro.l[1], l2);
  strcpy(quadro.l[2], l3);
  quadro.alarme[0] = classeFc == 3;
  quadro.alarme[1] = classeSpo2 == 3;
  quadro.alarme[2] = classeTemp == 3;
  quadro.piscando = estado == 3;
  quadro.rede = mqttConectado;
  portEXIT_CRITICAL(&muxTela);
}

void registraSerial() {
  const char* nomes[] = {"sem leitura", "normal", "AVISO", "ALARME"};
  Serial.printf("Ciclo: %lu ms | IR: %lu | BPM bruto: %.0f (correlacao %.2f, Maxim %ld) | BPM: %.0f | SpO2 bruto: %ld | SpO2: %.0f | Pele: %.2f C | Temp: %.2f C%s | Estado: %s | MQTT: %s\n",
                periodoCiclo, (unsigned long)ir[TAM - 1], bpmValido ? bpm : 0.0, qualidadeFc, (long)bpmMaxim, hBpm.n ? hBpm.mediana() : 0.0,
                (long)spo2, hSpo2.n ? hSpo2.mediana() : 0.0,
                hPele.n ? hPele.mediana() : 0.0, hPele.n ? temperatura() : 0.0,
                tempCalibrada ? "" : " (sem calibracao)", nomes[estado], mqttConectado ? "on" : "off");
}

void textoNumero(char* destino, size_t tam, bool existe, float valor, int casas) {
  if (existe) snprintf(destino, tam, "%.*f", casas, valor);
  else snprintf(destino, tam, "null");
}

void publicaLeituras() {
  const char* nomes[] = {"sem_leitura", "normal", "aviso", "alarme"};
  char sBpm[10], sSpo2[10], sTemp[10], sPele[10];
  textoNumero(sBpm, sizeof(sBpm), dedoAtual && hBpm.n >= 3, hBpm.n ? hBpm.mediana() : 0, 0);
  textoNumero(sSpo2, sizeof(sSpo2), dedoAtual && hSpo2.n >= 3, hSpo2.n ? hSpo2.mediana() : 0, 0);
  textoNumero(sTemp, sizeof(sTemp), tempCalibrada && hPele.n > 0, hPele.n ? temperatura() : 0, 2);
  textoNumero(sPele, sizeof(sPele), hPele.n > 0, hPele.n ? hPele.mediana() : 0, 2);

  char m[220];
  if (mqttConectado) {
    snprintf(m, sizeof(m), "{\"seq\":%lu,\"dedo\":%s,\"bpm\":%s,\"spo2\":%s,\"temp\":%s,\"pele\":%s,\"estado\":\"%s\"}",
             (unsigned long)++seqLeitura, dedoAtual ? "true" : "false", sBpm, sSpo2, sTemp, sPele, nomes[estado]);
    enfileira(T_LEITURAS, m, false, 0);
  }

  if (mqttConectado && estado != estadoPublicado) {
    estadoPublicado = estado;
    snprintf(m, sizeof(m), "{\"estado\":\"%s\",\"fc\":\"%s\",\"spo2\":\"%s\",\"temp\":\"%s\",\"bpm\":%s,\"spo2_valor\":%s,\"temp_valor\":%s}",
             nomes[estado], nomes[classeFc], nomes[classeSpo2], nomes[classeTemp], sBpm, sSpo2, sTemp);
    enfileira(T_ALERTA, m, false, 1);
  }

  if (mqttConectado && rodadasMedicao > 0) {
    rodadasMedicao = rodadasMedicao - 1;
    numeroRodada = numeroRodada + 1;
    int r = numeroRodada - 1;
    medicoes[0].tDeteccao[r] = tDeteccaoMax;
    medicoes[0].dedo[r] = dedoAtual;
    snprintf(m, sizeof(m), "{\"n\":%d,\"dedo\":%s,\"bpm\":%s,\"spo2\":%s}", numeroRodada, dedoAtual ? "true" : "false", sBpm, sSpo2);
    enfileira(T_MED_MAX, m, false, 0, 0, r);
    medicoes[1].tDeteccao[r] = tDeteccaoMlx;
    medicoes[1].dedo[r] = dedoAtual;
    snprintf(m, sizeof(m), "{\"n\":%d,\"pele\":%s}", numeroRodada, sPele);
    enfileira(T_MED_MLX, m, false, 0, 1, r);
  }
}

void finalizaCalibracao() {
  float soma = 0;
  for (int i = 0; i < PARES_CALIBRACAO; i++) soma += diferencas[i];
  float media = soma / PARES_CALIBRACAO;
  float var = 0;
  for (int i = 0; i < PARES_CALIBRACAO; i++) var += (diferencas[i] - media) * (diferencas[i] - media);
  float dp = sqrt(var / (PARES_CALIBRACAO - 1));
  nPares = 0;
  if (fabs(media) > 6.0) {
    Serial.printf("Correcao de %.2f C fora do plausivel. Calibracao descartada, repita.\n", media);
    return;
  }
  correcaoTemp = media;
  tempCalibrada = true;
  prefs.putFloat("correcao", correcaoTemp);
  prefs.putBool("calibrada", true);
  Serial.printf("Calibracao salva: correcao = %.2f C (desvio padrao das diferencas = %.2f C)\n", media, dp);
}

void trataComando(String c) {
  c.trim();
  if (c.startsWith("t ")) {
    float ref = c.substring(2).toFloat();
    if (ref < 35.0 || ref > 42.0) { Serial.println("Valor do termometro fora de 35,0 a 42,0 C. Use ponto: t 36.5"); return; }
    if (hPele.n < 5) { Serial.println("Aguarde 5 leituras do GY-906 com a testa a 1-2 cm do sensor."); return; }
    if (hPele.amplitude() > VARIACAO_MAX_LEITURA) { Serial.println("Leitura do GY-906 ainda variando. Mantenha a testa parada e tente de novo."); return; }
    float pele = hPele.mediana();
    if (pele < 28.0 || pele > 40.0) { Serial.printf("Leitura de %.2f C nao parece pele. Aproxime a testa do sensor.\n", pele); return; }
    diferencas[nPares++] = ref - pele;
    Serial.printf("Par %d de %d: termometro %.2f C, GY-906 %.2f C, diferenca %.2f C, ambiente %.1f C\n", nPares, PARES_CALIBRACAO, ref, pele, ref - pele, mlx.readAmbientTempC());
    if (nPares == PARES_CALIBRACAO) finalizaCalibracao();
  } else if (c == "apagar") {
    tempCalibrada = false;
    correcaoTemp = 0.0;
    nPares = 0;
    prefs.putBool("calibrada", false);
    prefs.putFloat("correcao", 0.0);
    Serial.println("Calibracao apagada. A temperatura saiu do alerta ate nova calibracao.");
  } else if (c.startsWith("correcao ")) {
    float v = c.substring(9).toFloat();
    if (fabs(v) > 6.0 || v == 0.0) { Serial.println("Correcao fora do plausivel. Use ponto: correcao 1.65"); return; }
    correcaoTemp = v;
    tempCalibrada = true;
    prefs.putFloat("correcao", correcaoTemp);
    prefs.putBool("calibrada", true);
    Serial.printf("Correcao gravada manualmente: %.2f C\n", correcaoTemp);
  } else if (c == "status") {
    if (tempCalibrada) Serial.printf("Calibrada, correcao = %.2f C\n", correcaoTemp);
    else Serial.printf("Sem calibracao, %d de %d pares registrados\n", nPares, PARES_CALIBRACAO);
  } else if (c.length()) {
    Serial.println("Comandos: t <temperatura do termometro>, correcao <valor>, status, apagar");
  }
}

void leSerial() {
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n' || ch == '\r') { if (comando.length()) trataComando(comando); comando = ""; }
    else if (comando.length() < 32) comando += ch;
  }
}

void processa(bool dedo) {
  periodoCiclo = ultimoCiclo ? millis() - ultimoCiclo : 0;
  ultimoCiclo = millis();
  dedoAtual = dedo;
  if (mlxOk) {
    float t = mlx.readObjectTempC();
    tDeteccaoMlx = micros();
    if (!isnan(t)) hPele.add(t);
  }
  if (!dedo) {
    dedoDesde = 0;
    hBpm.limpa();
    hSpo2.limpa();
  } else {
    if (dedoDesde == 0) { dedoDesde = millis(); ultimoBpmOk = ultimaSpo2Ok = millis(); }
    if (millis() - dedoDesde > ESTABILIZACAO_FC && bpmValido && bpm >= 40 && bpm <= 180) { hBpm.add(bpm); ultimoBpmOk = millis(); }
    if (millis() - dedoDesde > ESTABILIZACAO && spo2Valido && spo2 >= 80 && spo2 <= 100) { hSpo2.add(spo2); ultimaSpo2Ok = millis(); }
    if (millis() - dedoDesde < ESTABILIZACAO_FC) ultimoBpmOk = millis();
    if (millis() - ultimoBpmOk > VALIDADE_LEITURA) hBpm.limpa();
    if (millis() - ultimaSpo2Ok > VALIDADE_LEITURA) hSpo2.limpa();
  }
  int c = avalia(dedo);
  if (c == candidatoAnterior) repeticoes++;
  else { candidatoAnterior = c; repeticoes = 1; }
  if (repeticoes >= CONFIRMACOES || c == 0) estado = c;
  mostra();
  publicaLeituras();
  registraSerial();
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  travaLed = xSemaphoreCreateMutex();
  filaSaida = xQueueCreate(12, sizeof(Saida));
  tela.begin();
  escreveLed(0, 0, 0);
  strcpy(quadro.l[0], "Iniciando");
  quadro.l[1][0] = quadro.l[2][0] = '\0';

  prefs.begin("monitor", false);
  tempCalibrada = prefs.getBool("calibrada", true);
  correcaoTemp = prefs.getFloat("correcao", CORRECAO_INICIAL);

  Wire.begin(8, 9);
  maxOk = max30102.begin(Wire, I2C_SPEED_FAST);
  if (maxOk) max30102.setup(BRILHO_LED_SENSOR, 4, 2, 100, 411, 4096);
  ultimaAmostra = millis();

  Wire1.begin(4, 5, 100000);
  mlxOk = mlx.begin(0x5A, &Wire1);

  Serial.printf("MAX30102: %s | GY-906: %s\n", maxOk ? "OK" : "FALHOU", mlxOk ? "OK" : "FALHOU");
  xTaskCreatePinnedToCore(tarefaTela, "tela", 4096, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(tarefaRede, "rede", 8192, NULL, 1, NULL, 0);
  if (tempCalibrada) Serial.printf("Temperatura calibrada, correcao = %.2f C\n", correcaoTemp);
  else Serial.println("Temperatura sem calibracao. Comandos: t <temperatura do termometro>, correcao <valor>, status, apagar");
}

void iniciaMax() {
  maxOk = max30102.begin(Wire, I2C_SPEED_FAST);
  if (maxOk) max30102.setup(BRILHO_LED_SENSOR, 4, 2, 100, 411, 4096);
  n = novas = nFc = pFc = 0;
  bpmValido = false;
  spo2Valido = 0;
  ultimaAmostra = millis();
  Serial.printf("MAX30102 reiniciado: %s\n", maxOk ? "OK" : "FALHOU");
}

void loop() {
  atualizaLed();
  leSerial();

  if (!maxOk) {
    if (millis() - ultimaTela > 1000) { ultimaTela = millis(); processa(false); }
    if (millis() - ultimaAmostra > LIMITE_SEM_AMOSTRA) iniciaMax();
    return;
  }

  if (millis() - ultimaAmostra > LIMITE_SEM_AMOSTRA) {
    Serial.println("MAX30102 sem amostras");
    iniciaMax();
    processa(false);
    return;
  }

  max30102.check();
  while (max30102.available()) {
    ultimaAmostra = millis();
    if (n == TAM) {
      memmove(red, red + 25, (TAM - 25) * sizeof(uint32_t));
      memmove(ir, ir + 25, (TAM - 25) * sizeof(uint32_t));
      n = TAM - 25;
    }
    red[n] = max30102.getFIFORed();
    ir[n] = max30102.getFIFOIR();
    irFc[pFc] = ir[n];
    pFc = (pFc + 1) % TAM_FC;
    if (nFc < TAM_FC) nFc++;
    max30102.nextSample();
    n++;
    novas++;
  }

  if (n == TAM && novas >= 25) {
    novas = 0;
    tDeteccaoMax = micros();
    bool dedo = ir[TAM - 1] > 50000;
    if (dedo) {
      maxim_heart_rate_and_oxygen_saturation(ir, TAM, red, &spo2, &spo2Valido, &bpmMaxim, &bpmMaximValido);
      bpmValido = calculaFc();
    } else {
      bpmValido = false;
      spo2Valido = 0;
      spo2 = 0;
      bpmMaxim = 0;
      nFc = pFc = 0;
      qualidadeFc = 0;
    }
    processa(dedo);
  }
}
