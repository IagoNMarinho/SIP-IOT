#define ENABLE_USER_AUTH
#define ENABLE_DATABASE

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <FirebaseClient.h>

//credenciais
#define WIFI_SSID "Caroline"
#define WIFI_PASSWORD "1356903030"

#define Web_API_KEY "AIzaSyBkQnjqpGgCBWi1RKoJ_-ngsqIah8nUDn4"           //chave de api do projeto firebase
#define DATABASE_URL "https://projet0sip-default-rtdb.firebaseio.com/"  //url do banco de dados

//usuário de email do firebase e senha correspondente
#define USER_EMAIL "marinhoiago96@gmail.com"
#define USER_PASS "Familia5809*"

void processData(AsyncResult &aResult);

UserAuth user_auth(Web_API_KEY, USER_EMAIL, USER_PASS);  //objeto de autenticação usando chave de api, email e senha

//componentes do Firebase
FirebaseApp app;
WiFiClientSecure ssl_client;
using AsyncClient = AsyncClientClass;
AsyncClient aClient(ssl_client);
RealtimeDatabase Database;

//variaveis de tempo para envio de dados a cada 10 segundos
unsigned long lastSendTime = 0;
const unsigned long sendInterval = 10000;
int intValue = 0;
float floatValue = 0.01;
String stringValue = "";

bool dispositivoAtivo = true;

#define TdsSensorPin 4  //pino TDS
#define VREF 3.3        //valor de voltagem
#define SCOUNT 30       //numero de amostras filtradas para obter um valor

//vetores para armazenar as leituras
int analogBuffer[SCOUNT];  //armazena o valor no vetor, lido em ADC
int analogBufferTemp[SCOUNT];

//variaveis de indice para percorrer os vetores
int analogBufferIndex = 0;
int copyIndex = 0;

//variaveis definidas para calculos
float averageVoltage = 0;
float tdsValue = 0;

//temperatura definida para compensacao (dps trocar pelo sensor de temp)
float temperature = 25;

//obtem um valor de TDS estavel dps de um conjunto de leituras (algoritmo de mediana)
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

void setup() {
  Serial.begin(115200);

  //conectar ao wifi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Conectando ao WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(300);
  }
  Serial.println();

  configTime(-3 * 3600, 0, "pool.ntp.org", "time.nist.gov");  //sincroniza o relogio pela internet (fuso de Brasilia, UTC-3)

  ssl_client.setInsecure();
  ssl_client.setConnectionTimeout(1000);
  ssl_client.setHandshakeTimeout(5);

  // Inicializa o Firebase
  initializeApp(aClient, app, getAuth(user_auth), processData, "authTask");
  app.getApp<RealtimeDatabase>(Database);
  Database.url(DATABASE_URL);


  pinMode(TdsSensorPin, INPUT);
  analogReadResolution(12);                         //ADC de 12 bits (0 a 4095)
  analogSetPinAttenuation(TdsSensorPin, ADC_11db);  //faixa de leitura até ~3,1 V
}

void loop() {

  app.loop();

  //tera novas leituras de TDS a cada 40 milésimos, serao salvas no buffer
  static unsigned long analogSampleTimepoint = millis();
  if (millis() - analogSampleTimepoint > 40U) {
    analogSampleTimepoint = millis();
    analogBuffer[analogBufferIndex] = analogRead(TdsSensorPin);
    analogBufferIndex++;
    if (analogBufferIndex == SCOUNT) {
      analogBufferIndex = 0;
    }
  }

  //a cada 800 milésimos com as leituras mais recentes calcula a tensao media com o algoritmo de mediana
  static unsigned long printTimepoint = millis();
  if (millis() - printTimepoint > 800U) {
    printTimepoint = millis();
    for (copyIndex = 0; copyIndex < SCOUNT; copyIndex++) {
      analogBufferTemp[copyIndex] = analogBuffer[copyIndex];
    }

    //le o valor mais estavel pelo algoritmo de mediana e converte para voltagem
    averageVoltage = getMedianNum(analogBufferTemp, SCOUNT) * (float)VREF / 4096.0;

    //calcula o coeficiente de compensacao de temperatura
    float compensacaoCoeficiente = 1.0 + 0.02 * (temperature - 25.0);
    float compensacaoVoltage = averageVoltage / compensacaoCoeficiente;

    tdsValue = (133.42 * compensacaoVoltage * compensacaoVoltage * compensacaoVoltage - 255.86 * compensacaoVoltage * compensacaoVoltage + 857.39 * compensacaoVoltage) * 0.5;
  }

  if (app.ready()) {

    // envio peridico de dados a cada 10 segundos
    unsigned long currentTime = millis();
    if (currentTime - lastSendTime >= sendInterval) {
      // atualiza o horario
      lastSendTime = currentTime;
      
      //consulta o comando de conectar/desconectar vindo do site (roda primeiro, sincrono, pra nao disputar com os envios)
      String comando = Database.get<String>(aClient, "/sensores/dispositivo-001/controle/ativo");
      comando.trim();             //remove espacos/quebras de linha nas pontas
      comando.replace("\"", "");  //remove aspas, caso o valor venha entre aspas

      Serial.print("comando recebido: [");
      Serial.print(comando);
      Serial.println("]");

      if (comando == "true") dispositivoAtivo = true;
      if (comando == "false") dispositivoAtivo = false;

      time_t hora = time(nullptr);
      struct tm info;
      bool horaValida = getLocalTime(&info);

      if (dispositivoAtivo) {
        Database.set<int>(aClient, "/sensores/dispositivo-001/SensorTDS/valor", (int)tdsValue, processData, "RTDB_Send_String");
        if (getLocalTime(&info)) {
          char horaTexto[20];
          strftime(horaTexto, sizeof(horaTexto), "%d/%m/%Y %H:%M:%S", &info);
          Database.set<String>(aClient, "/sensores/dispositivo-001/SensorTDS/hora", String(horaTexto), processData, "RTDB_Send_Hora");
        }
      }

      //sinal de vida que continua sendo enviado mesmo pausado, para o site distinguir "pausado" de "offline"
      if (horaValida) {
        char horaVida[20];
        strftime(horaVida, sizeof(horaVida), "%d/%m/%Y %H:%M:%S", &info);
        Database.set<String>(aClient, "/sensores/dispositivo-001/ultimoContato", String(horaVida), processData, "RTDB_Send_Contato");
      }
    }
  }
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

  if (aResult.available())
    Firebase.printf("task: %s, payload: %s\n", aResult.uid().c_str(), aResult.c_str());
}