#define ENABLE_USER_AUTH
#define ENABLE_DATABASE

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <FirebaseClient.h>

#include <OneWire.h>
#include <DallasTemperature.h>

//credenciais
#define WIFI_SSID "Caroline"
#define WIFI_PASSWORD "1356903030"

#define Web_API_KEY "AIzaSyBkQnjqpGgCBWi1RKoJ_-ngsqIah8nUDn4"    //chave de api do projeto firebase
#define DATABASE_URL "https://projet0sip-default-rtdb.firebaseio.com/"   //url do banco de dados

//usuário de email do firebase e senha correspondente
#define USER_EMAIL "marinhoiago96@gmail.com"
#define USER_PASS "Familia5809*"

void processData(AsyncResult &aResult);
void enviarColeta(String horaTexto);

UserAuth user_auth(Web_API_KEY, USER_EMAIL, USER_PASS);

//componentes do Firebase
FirebaseApp app;
WiFiClientSecure ssl_client;
using AsyncClient = AsyncClientClass;
AsyncClient aClient(ssl_client);
RealtimeDatabase Database;

//variaveis de tempo para envio de dados do monitoramento contínuo
unsigned long lastMonitorSendTime = 0;
unsigned long intervaloMonitoramentoMs = 10000;  //10s padrao, pode ser sobrescrito pelo site

//ciclo rapido, independente do intervalo de monitoramento
unsigned long lastCicloRapido = 0;
const unsigned long cicloRapidoMs = 3000;

bool dispositivoAtivo = true;

//o processData marca aqui quando o site pediu leitura imediata, e o loop trata
bool leituraImediataPendente = false;

// ---------------- SENSOR TDS ----------------
#define TdsSensorPin 4  //pino TDS
#define VREF 3.3        //valor de voltagem
#define SCOUNT 30       //numero de amostras filtradas para obter um valor

int analogBuffer[SCOUNT];  //leituras brutas do ADC (0 a 4095)
int analogBufferTemp[SCOUNT];

int analogBufferIndex = 0;
int copyIndex = 0;

float averageVoltage = 0;
float tdsValue = 0;

// ---------------- SENSOR TEMPERATURA ----------------
#define TempSensorPin 5  //pino Temperatura

OneWire oneWire(TempSensorPin);
DallasTemperature sensors(&oneWire);

//temperatura usada na compensacao (mantem o ultimo valor valido; comeca em 25)
float temperature = 25.0;
bool temperaturaValida = false;  //vira true apos a primeira leitura valida

//obtem um valor estavel apos um conjunto de leituras (algoritmo de mediana)
int getMedianNum(int bArray[], int iFilterLen) {
  int bTab[iFilterLen];
  for (byte i = 0; i < iFilterLen; i++)
    bTab[i] = bArray[i];
  int i, j, bTemp;
  for (j = 0; j < iFilterLen - 1; j++) {
    for (i = 0; i < iFilterLen - j - 1; i++) {
      if (bTab[i] > bTab[i + 1]) {
        bTemp = bTab[i];
        bTab[i] = bTab[i + 1];
        bTab[i + 1] = bTemp;
      }
    }
  }
  if ((iFilterLen & 1) > 0) {
    bTemp = bTab[(iFilterLen - 1) / 2];
  } else {
    bTemp = (bTab[iFilterLen / 2] + bTab[iFilterLen / 2 - 1]) / 2;
  }
  return bTemp;
}

// ---------------- SENSOR TURBIDEZ ----------------
#define TurbSensorPin 6

const int N_PONTOS = 3;
float tabelaV[N_PONTOS]   = {1.0, 0.67, 0.36};
float tabelaNTU[N_PONTOS] = {0, 200, 3000};

float tensaoTurbidez = 0;
float ntuValue = 0;

long turbSoma = 0;
float turbContagem = 0;

float tensaoParaNTU(float v) {
  if (v >= tabelaV[0]) return tabelaNTU[0];
  if (v <= tabelaV[N_PONTOS - 1]) return tabelaNTU[N_PONTOS - 1];

  for (int i = 0; i < N_PONTOS - 1; i++) {
    if (v <= tabelaV[i] && v >= tabelaV[i + 1]) {
      float frac = (tabelaV[i] - v) / (tabelaV[i] - tabelaV[i + 1]);
      return tabelaNTU[i] + frac * (tabelaNTU[i + 1] - tabelaNTU[i]);
    }
  }
  return tabelaNTU[N_PONTOS - 1];
}

void setup() {
  Serial.begin(115200);

  //sensor de temperatura
  sensors.begin();
  sensors.setResolution(12);            //12 bits = maior precisao (conversao ~750 ms)
  sensors.setWaitForConversion(false);  //nao trava o loop esperando a conversao
  sensors.requestTemperatures();        //primeira conversao ja fica pronta no primeiro ciclo
  Serial.print("Sensores de temperatura encontrados: ");
  Serial.println(sensors.getDeviceCount());

  //conectar ao wifi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Conectando ao WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(300);
  }
  Serial.println();

  configTime(-3 * 3600, 0, "pool.ntp.org", "time.nist.gov");  //UTC-3

  ssl_client.setInsecure();
  ssl_client.setConnectionTimeout(1000);
  ssl_client.setHandshakeTimeout(5);

  //inicializa o Firebase
  initializeApp(aClient, app, getAuth(user_auth), processData, "authTask");
  app.getApp<RealtimeDatabase>(Database);
  Database.url(DATABASE_URL);

  //TDS
  pinMode(TdsSensorPin, INPUT);
  analogReadResolution(12);                         //ADC de 12 bits (0 a 4095)
  analogSetPinAttenuation(TdsSensorPin, ADC_11db);  //faixa de leitura até ~3,1 V

  //turbidez
  analogSetPinAttenuation(TurbSensorPin, ADC_11db);
}

void loop() {

  app.loop();

  //novas leituras de TDS a cada 40 ms, salvas no buffer
  static unsigned long analogSampleTimepoint = millis();
  if (millis() - analogSampleTimepoint > 40U) {
    analogSampleTimepoint = millis();
    analogBuffer[analogBufferIndex] = analogRead(TdsSensorPin);
    analogBufferIndex++;
    if (analogBufferIndex == SCOUNT) {
      analogBufferIndex = 0;
    }
  }

   static unsigned long turbSampleTimepoint = millis();
  if (millis() - turbSampleTimepoint >= 5U) {
    turbSampleTimepoint = millis();
    turbSoma += analogReadMilliVolts(TurbSensorPin);
    turbContagem++;

    if (turbContagem == 20) {
      float leitura = turbSoma / 20.0;
      tensaoTurbidez = leitura / 1000.0;
      ntuValue = tensaoParaNTU(tensaoTurbidez);
      turbSoma = 0;
      turbContagem = 0;
    }
  }

  //a cada 800 ms: le a temperatura, calcula a tensao mediana e o TDS compensado
  static unsigned long printTimepoint = millis();
  if (millis() - printTimepoint > 800U) {
    printTimepoint = millis();

    //le a temperatura pedida no ciclo anterior e ja pede a proxima
    float novaTemperatura = sensors.getTempCByIndex(0);
    sensors.requestTemperatures();

    //descarta desconectado (-127), "ligou/falhou" (85) e qualquer valor fora de uma faixa plausivel
    if (novaTemperatura == DEVICE_DISCONNECTED_C || novaTemperatura == 85.0 ||
        novaTemperatura < -5.0 || novaTemperatura > 80.0) {
      Serial.println("Aviso: leitura de temperatura invalida, mantendo o ultimo valor");
    } else {
      temperature = novaTemperatura;
      temperaturaValida = true;
    }

    for (copyIndex = 0; copyIndex < SCOUNT; copyIndex++) {
      analogBufferTemp[copyIndex] = analogBuffer[copyIndex];
    }

    //valor mais estavel pela mediana, convertido para voltagem
    averageVoltage = getMedianNum(analogBufferTemp, SCOUNT) * (float)VREF / 4096.0;

    //compensacao de temperatura
    float compensacaoCoeficiente = 1.0 + 0.02 * (temperature - 25.0);
    float compensacaoVoltage = averageVoltage / compensacaoCoeficiente;

    tdsValue = (133.42 * compensacaoVoltage * compensacaoVoltage * compensacaoVoltage
                - 255.86 * compensacaoVoltage * compensacaoVoltage
                + 857.39 * compensacaoVoltage) * 0.5;
    if (tdsValue < 0) tdsValue = 0;

    //monitor serial para depuracao
    Serial.printf("Temp: %.2f C (%s) | Tensao: %.3f V | TDS: %.2f ppm | Turb: %.2f V | NTU: %.0f\n",
                  temperature, temperaturaValida ? "ok" : "padrao 25", averageVoltage, tdsValue,
                  tensaoTurbidez, ntuValue);
  }

  if (app.ready()) {

    unsigned long agoraMs = millis();

    if (agoraMs - lastCicloRapido >= cicloRapidoMs) {
      lastCicloRapido = agoraMs;

      struct tm info;
      bool horaValida = getLocalTime(&info, 10);  //timeout curto para nao travar o loop

      //trata o comando recebido no ciclo anterior (as respostas chegam no processData)
      if (leituraImediataPendente) {
        leituraImediataPendente = false;
        if (horaValida) {
          char horaTexto[20];
          strftime(horaTexto, sizeof(horaTexto), "%d/%m/%Y %H:%M:%S", &info);
          enviarColeta(String(horaTexto));
        }
        //consome o comando para nao repetir no proximo ciclo
        Database.set<String>(aClient, "/sensores/dispositivo-001/controle/comando", "nenhum", processData, "RTDB_Send_Comando");
      }

      //consultas assincronas, cada resposta cai no processData (identificada pelo uid)
      Database.get(aClient, "/sensores/dispositivo-001/controle/ativo", processData, false, "RTDB_Get_Ativo");
      Database.get(aClient, "/sensores/dispositivo-001/controle/intervaloMonitoramento", processData, false, "RTDB_Get_Intervalo");
      Database.get(aClient, "/sensores/dispositivo-001/controle/comando", processData, false, "RTDB_Get_Comando");

      //sinal de vida (distingue "pausado" de "offline" no site)
      if (horaValida) {
        char horaVida[20];
        strftime(horaVida, sizeof(horaVida), "%d/%m/%Y %H:%M:%S", &info);
        Database.set<String>(aClient, "/sensores/dispositivo-001/ultimoContato", String(horaVida), processData, "RTDB_Send_Contato");
      }
    }

    //envio periodico do monitoramento continuo
    if (dispositivoAtivo && agoraMs - lastMonitorSendTime >= intervaloMonitoramentoMs) {
      lastMonitorSendTime = agoraMs;

      struct tm info;
      if (getLocalTime(&info, 10)) {
        char horaTexto[20];
        strftime(horaTexto, sizeof(horaTexto), "%d/%m/%Y %H:%M:%S", &info);
        enviarColeta(String(horaTexto));
      }
    }
  }
}

//envia TDS, temperatura, turbidez e hora juntos, numa unica gravacao
void enviarColeta(String horaTexto) {
  object_t coleta, objTDS, objTemp, objTurb, objHora;
  JsonWriter writer;

  writer.create(objTDS, "SensorTDS", number_t(tdsValue, 2));
  writer.create(objTemp, "SensorTemp", number_t(temperature, 2));
  writer.create(objTurb, "SensorTurbidez", number_t(ntuValue, 0));
  writer.create(objHora, "hora", string_t(horaTexto));

  writer.join(coleta, 4, objTDS, objTemp, objTurb, objHora);

  Database.set<object_t>(aClient, "/sensores/dispositivo-001/Coleta", coleta, processData, "RTDB_Send_Coleta");
}

void processData(AsyncResult &aResult) {
  if (!aResult.isResult())
    return;

  if (aResult.isEvent())
    Firebase.printf("Event task: %s, msg: %s, code: %d\n", aResult.uid().c_str(), aResult.eventLog().message().c_str(), aResult.eventLog().code());

  if (aResult.isDebug())
    Firebase.printf("Debug task: %s, msg: %s\n", aResult.uid().c_str(), aResult.debug().c_str());

  if (aResult.isError())
    Firebase.printf("Error task: %s, msg: %s, code: %d\n", aResult.uid().c_str(), aResult.error().message().c_str(), aResult.error().code());

  if (aResult.available()) {
    Firebase.printf("task: %s, payload: %s\n", aResult.uid().c_str(), aResult.c_str());

    String uid = aResult.uid();
    String valor = aResult.c_str();
    valor.trim();
    valor.replace("\"", "");

    if (uid == "RTDB_Get_Ativo") {
      if (valor == "true") dispositivoAtivo = true;
      if (valor == "false") dispositivoAtivo = false;
    }

    if (uid == "RTDB_Get_Intervalo") {
      int segundosIntervalo = valor.toInt();
      if (segundosIntervalo > 0) intervaloMonitoramentoMs = (unsigned long)segundosIntervalo * 1000UL;
    }

    if (uid == "RTDB_Get_Comando") {
      if (valor == "imediata") leituraImediataPendente = true;
    }
  }
}