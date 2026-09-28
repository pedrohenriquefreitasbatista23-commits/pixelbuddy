/*
  Pixel Buddy — olhos estilo Neo (versão fluida p/ Wokwi)
  ========================================================
  Hardware: ESP32 + ILI9341 240x320 + 3 botões + buzzer

  Otimizado para Wokwi:
  - redesenho ~20 fps (50 ms)
  - menos formas por frame
  - animações reduzidas (pisca + olhar ocasional)
  - bug de offset de posição corrigido

  Botões (Wokwi: teclas 1 / 2 / 3):
  - COR    GPIO15  cicla a cor
  - RESET  GPIO17  reinicia
  - ESTUDO GPIO16  cronômetro
*/

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>

#define TFT_CS   5
#define TFT_DC   2
#define TFT_RST  4

#define PINO_BOTAO_COR    15
#define PINO_BOTAO_RESET  17
#define PINO_BOTAO_ESTUDO 16
#define PINO_BUZZER       25

Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);

#define TELA_LARGURA 240
#define TELA_ALTURA  320

const uint16_t PALETA_CORES[] = {
  0x5FF9,          // Menta (Neo)
  ILI9341_CYAN,
  ILI9341_YELLOW,
  ILI9341_MAGENTA,
  ILI9341_GREEN,
  0xFC00,          // Laranja
  ILI9341_BLUE,
  ILI9341_WHITE,
};
const int TOTAL_CORES = sizeof(PALETA_CORES) / sizeof(PALETA_CORES[0]);
int indiceCorAtual = 0;

#define COR_FUNDO ILI9341_BLACK
#define COR_TEXTO ILI9341_WHITE

enum EstadoTela { TELA_LOGO, TELA_TRANQUILO, TELA_AVISO };
EstadoTela estadoAtual = TELA_LOGO;

enum Expressao { FELIZ, SONOLENTO, SURPRESO, ANIMADO, NEUTRO, BRAVO };
Expressao expressaoAtual = NEUTRO;

int indiceLembreteAtual = 0;
const char* NOMES_LEMBRETE[] = { "Hora de beber agua!", "Descanse a vista", "Hora de se alongar!" };
Expressao EXPRESSOES_LEMBRETE[] = { SURPRESO, SONOLENTO, ANIMADO };

unsigned long tempoInicioEstado = 0;
const unsigned long TEMPO_INICIAL_MS = 5000;
const unsigned long TEMPO_TROCA_MS   = 20000;

bool estudando = false;
unsigned long inicioEstudoMs = 0;
int ultimoSegundoMostrado = -1;

volatile bool flagCor = false;
volatile bool flagReset = false;
volatile bool flagEstudo = false;
unsigned long ultimaInterrupcaoCor = 0;
unsigned long ultimaInterrupcaoReset = 0;
unsigned long ultimaInterrupcaoEstudo = 0;
const unsigned long DEBOUNCE_ISR_MS = 250;

bool alertaAtivo = false;
unsigned long alertaInicioMs = 0;
int alertaFase = 0;

int ultimoTextoId = -1;

// ------------------------------------------------------------
// Motor dos olhos (coordenadas lógicas 128x64 → escala 15/8)
// ------------------------------------------------------------
#define LOG_W 128
#define LOG_H 64
#define FACE_Y 28

const int EYE_W = 36;
const int EYE_H = 28;
const int EYE_R = 10;
const int EYE_SPACE = 10;

// estado atual (desenhado)
int lX, lY, lW, lH;
int rX, rY, rW, rH;
// alvo
int lXN, lYN, lWN, lHN;
int rXN, rYN, rWN, rHN;

bool idleOn = true;
bool autoBlink = true;
unsigned long blinkAt = 0;
unsigned long idleAt = 0;
unsigned long fpsTimer = 0;

// lids (simplificados: 0 = aberto, >0 = cobertura)
int lidTired = 0, lidTiredN = 0;
int lidHappy = 0, lidHappyN = 0;

inline int lerpI(int a, int b) {
  // aproximação lenta → menos frames intermediários, mais estável no Wokwi
  if (a == b) return a;
  int d = b - a;
  if (d > 0) return a + max(1, d / 3);
  return a + min(-1, d / 3);
}

inline int mx(int x) { return (x * 15) / 8; }
inline int my(int y) { return FACE_Y + (y * 15) / 8; }
inline int ms(int v) { return max(1, (v * 15) / 8); }

int maxX() { return LOG_W - EYE_W * 2 - EYE_SPACE; }
int maxY() { return LOG_H - EYE_H; }

void setLook(int dir) {
  int mx_ = maxX();
  int my_ = maxY();
  int x = mx_ / 2;
  int y = my_ / 2;
  switch (dir) {
    case 1: x = mx_ / 2; y = 0;      break; // N
    case 2: x = mx_;     y = 0;      break; // NE
    case 3: x = mx_;     y = my_/2;  break; // E
    case 4: x = mx_;     y = my_;    break; // SE
    case 5: x = mx_ / 2; y = my_;    break; // S
    case 6: x = 0;       y = my_;    break; // SW
    case 7: x = 0;       y = my_/2;  break; // W
    case 8: x = 0;       y = 0;      break; // NW
    default: break;                          // centro
  }
  lXN = x;
  lYN = y;
}

void setMoodEyes(Expressao ex) {
  expressaoAtual = ex;
  if (ex == SURPRESO) {
    lHN = 42; rHN = 42;
    lWN = 30; rWN = 30;
  } else if (ex == SONOLENTO) {
    lHN = 14; rHN = 14;
    lWN = EYE_W; rWN = EYE_W;
  } else {
    lHN = EYE_H; rHN = EYE_H;
    lWN = EYE_W; rWN = EYE_W;
  }

  if (ex == SONOLENTO) setLook(5);   // olha pra baixo
  else setLook(0);

  lidTiredN = (ex == SONOLENTO) ? 8 : 0;
  lidHappyN = (ex == FELIZ || ex == ANIMADO) ? 10 : 0;
}

void blinkEyes() {
  lHN = 2;
  rHN = 2;
}

void bootEyes(unsigned long agora) {
  lW = rW = EYE_W;
  lH = rH = EYE_H;
  lWN = rWN = EYE_W;
  lHN = rHN = EYE_H;
  lX = lXN = maxX() / 2;
  lY = lYN = maxY() / 2;
  rX = lX + EYE_W + EYE_SPACE;
  rY = lY;
  rXN = rX; rYN = rY;
  lidTired = lidTiredN = 0;
  lidHappy = lidHappyN = 0;
  idleOn = true;
  autoBlink = true;
  blinkAt = agora + 2500;
  idleAt  = agora + 2000;
  expressaoAtual = NEUTRO;
}

void stepEyes(unsigned long agora) {
  // piscar
  if (autoBlink && agora >= blinkAt) {
    blinkEyes();
    blinkAt = agora + 2800 + random(0, 3200);
  }

  // olhar ocasional (bem espaçado)
  if (idleOn && agora >= idleAt && !estudando) {
    // só 5 direções simples + centro
    static const int dirs[] = {0, 3, 7, 1, 5};
    setLook(dirs[random(0, 5)]);
    idleAt = agora + 3500 + random(0, 4000);
  }

  // lerp de tamanho
  lH = lerpI(lH, lHN);
  rH = lerpI(rH, rHN);
  lW = lerpI(lW, lWN);
  rW = lerpI(rW, rWN);

  // depois de fechar o piscar, reabre para o tamanho do mood
  if (lH <= 3) {
    if (expressaoAtual == SURPRESO)     lHN = 42;
    else if (expressaoAtual == SONOLENTO) lHN = 14;
    else                                  lHN = EYE_H;
  }
  if (rH <= 3) {
    if (expressaoAtual == SURPRESO)     rHN = 42;
    else if (expressaoAtual == SONOLENTO) rHN = 14;
    else                                  rHN = EYE_H;
  }

  // posição alvo do olho direito acompanha o esquerdo
  rXN = lXN + lW + EYE_SPACE;
  rYN = lYN;

  lX = lerpI(lX, lXN);
  lY = lerpI(lY, lYN);
  rX = lerpI(rX, rXN);
  rY = lerpI(rY, rYN);

  // lids
  lidTired = lerpI(lidTired, lidTiredN);
  lidHappy = lerpI(lidHappy, lidHappyN);
}

void desenharOlhos(uint16_t cor) {
  // limpa só a faixa dos olhos
  tft.fillRect(0, FACE_Y, TELA_LARGURA, ms(LOG_H) + 4, COR_FUNDO);

  // posição de desenho: centraliza verticalmente dentro do espaço do olho
  int dlY = lY + (EYE_H - lH) / 2;
  int drY = rY + (EYE_H - rH) / 2;

  // olhos
  int rrL = min(EYE_R, min(lW, lH) / 2);
  int rrR = min(EYE_R, min(rW, rH) / 2);
  tft.fillRoundRect(mx(lX), my(dlY), ms(lW), ms(lH), ms(rrL), cor);
  tft.fillRoundRect(mx(rX), my(drY), ms(rW), ms(rH), ms(rrR), cor);

  // pálpebra cansada (triângulo simples no topo)
  if (lidTired > 2) {
    tft.fillTriangle(
      mx(lX), my(dlY),
      mx(lX + lW), my(dlY),
      mx(lX), my(dlY + lidTired),
      COR_FUNDO);
    tft.fillTriangle(
      mx(rX), my(drY),
      mx(rX + rW), my(drY),
      mx(rX + rW), my(drY + lidTired),
      COR_FUNDO);
  }

  // sorriso (corta a parte de baixo)
  if (lidHappy > 2) {
    tft.fillRect(mx(lX - 1), my(dlY + lH - lidHappy), ms(lW + 2), ms(lidHappy + 2), COR_FUNDO);
    tft.fillRect(mx(rX - 1), my(drY + rH - lidHappy), ms(rW + 2), ms(lidHappy + 2), COR_FUNDO);
  }

  // boca simples (só quando olhos abertos e não feliz/animado)
  bool fechado = (lH < 5 && rH < 5);
  if (!fechado && expressaoAtual != FELIZ && expressaoAtual != ANIMADO) {
    int minBottom = max(dlY + lH, drY + rH);
    int midX = (lX + lW + rX) / 2;
    if (expressaoAtual == SURPRESO) {
      tft.fillCircle(mx(midX), my(minBottom + 6), ms(4), cor);
    } else {
      // linha reta simples (muito mais barata que roundRect)
      tft.fillRect(mx(midX - 5), my(minBottom + 3), ms(10), ms(2), cor);
    }
  }
}

// ------------------------------------------------------------
// ISRs
// ------------------------------------------------------------
void IRAM_ATTR isrBotaoCor() {
  unsigned long agora = millis();
  if (agora - ultimaInterrupcaoCor > DEBOUNCE_ISR_MS) {
    flagCor = true;
    ultimaInterrupcaoCor = agora;
  }
}
void IRAM_ATTR isrBotaoReset() {
  unsigned long agora = millis();
  if (agora - ultimaInterrupcaoReset > DEBOUNCE_ISR_MS) {
    flagReset = true;
    ultimaInterrupcaoReset = agora;
  }
}
void IRAM_ATTR isrBotaoEstudo() {
  unsigned long agora = millis();
  if (agora - ultimaInterrupcaoEstudo > DEBOUNCE_ISR_MS) {
    flagEstudo = true;
    ultimaInterrupcaoEstudo = agora;
  }
}

void executarReset();
void alternarModoEstudo(unsigned long agora);
void tocarAlerta();

void setup() {
  Serial.begin(115200);
  randomSeed(esp_random());

  pinMode(PINO_BOTAO_COR, INPUT_PULLUP);
  pinMode(PINO_BOTAO_RESET, INPUT_PULLUP);
  pinMode(PINO_BOTAO_ESTUDO, INPUT_PULLUP);
  pinMode(PINO_BUZZER, OUTPUT);
  digitalWrite(PINO_BUZZER, LOW);

  attachInterrupt(digitalPinToInterrupt(PINO_BOTAO_COR), isrBotaoCor, FALLING);
  attachInterrupt(digitalPinToInterrupt(PINO_BOTAO_RESET), isrBotaoReset, FALLING);
  attachInterrupt(digitalPinToInterrupt(PINO_BOTAO_ESTUDO), isrBotaoEstudo, FALLING);

  tft.begin();
  tft.setRotation(0);

  executarReset();
}

void loop() {
  unsigned long agora = millis();

  if (flagCor) {
    flagCor = false;
    indiceCorAtual = (indiceCorAtual + 1) % TOTAL_CORES;
    tone(PINO_BUZZER, 2000, 40);
    ultimoTextoId = -1;
  }
  if (flagReset) {
    flagReset = false;
    executarReset();
  }
  if (flagEstudo) {
    flagEstudo = false;
    alternarModoEstudo(agora);
  }

  if (alertaAtivo) {
    unsigned long decorrido = agora - alertaInicioMs;
    if (alertaFase == 0 && decorrido >= 200) {
      tone(PINO_BUZZER, 1400, 200);
      alertaFase = 1;
    } else if (alertaFase == 1 && decorrido >= 450) {
      alertaAtivo = false;
    }
  }

  // máquina de estados de tela
  if (!estudando) {
    unsigned long limite = (estadoAtual == TELA_LOGO) ? TEMPO_INICIAL_MS : TEMPO_TROCA_MS;
    if (agora - tempoInicioEstado >= limite) {
      tempoInicioEstado = agora;
      if (estadoAtual == TELA_LOGO) {
        estadoAtual = TELA_TRANQUILO;
        setMoodEyes(NEUTRO);
        idleOn = true;
      } else if (estadoAtual == TELA_TRANQUILO) {
        estadoAtual = TELA_AVISO;
        setMoodEyes(EXPRESSOES_LEMBRETE[indiceLembreteAtual]);
        idleOn = false;
        tocarAlerta();
      } else {
        indiceLembreteAtual = (indiceLembreteAtual + 1) % 3;
        estadoAtual = TELA_TRANQUILO;
        setMoodEyes(NEUTRO);
        idleOn = true;
      }
      ultimoTextoId = -1;
    }
  }

  // ~20 fps — muito mais leve no Wokwi
  if (agora - fpsTimer >= 50) {
    fpsTimer = agora;
    stepEyes(agora);
    uint16_t cor = PALETA_CORES[indiceCorAtual];
    desenharOlhos(cor);

    if (estudando) {
      unsigned long segundosTotais = (agora - inicioEstudoMs) / 1000;
      if ((int)segundosTotais != ultimoSegundoMostrado || ultimoTextoId != 90) {
        ultimoSegundoMostrado = (int)segundosTotais;
        ultimoTextoId = 90;
        tft.fillRect(0, FACE_Y + ms(LOG_H) + 4, TELA_LARGURA,
                     TELA_ALTURA - (FACE_Y + ms(LOG_H) + 4), COR_FUNDO);
        tft.setTextSize(2);
        tft.setTextColor(COR_TEXTO);
        const char* rotulo = "Estudando";
        tft.setCursor((TELA_LARGURA - (int)strlen(rotulo) * 12) / 2, 200);
        tft.print(rotulo);
        char cronometro[6];
        sprintf(cronometro, "%02d:%02d", (int)(segundosTotais / 60), (int)(segundosTotais % 60));
        tft.setTextSize(5);
        tft.setTextColor(cor);
        tft.setCursor((TELA_LARGURA - (int)strlen(cronometro) * 30) / 2, 236);
        tft.print(cronometro);
      }
    } else if (estadoAtual == TELA_LOGO) {
      unsigned long decorrido = agora - tempoInicioEstado;
      if (decorrido > TEMPO_INICIAL_MS) decorrido = TEMPO_INICIAL_MS;
      int percentual = (int)((decorrido * 100) / TEMPO_INICIAL_MS);

      if (ultimoTextoId != 10) {
        ultimoTextoId = 10;
        tft.fillRect(0, FACE_Y + ms(LOG_H) + 4, TELA_LARGURA,
                     TELA_ALTURA - (FACE_Y + ms(LOG_H) + 4), COR_FUNDO);
        tft.setTextSize(2);
        tft.setTextColor(COR_TEXTO);
        const char* t = "Pixel Buddy";
        tft.setCursor((TELA_LARGURA - (int)strlen(t) * 12) / 2, TELA_ALTURA - 90);
        tft.print(t);
        int larguraBarra = 160, alturaBarra = 14;
        int startX = (TELA_LARGURA - larguraBarra) / 2;
        int startY = TELA_ALTURA - 45;
        tft.drawRect(startX, startY, larguraBarra, alturaBarra, cor);
        tft.drawRect(startX + 1, startY + 1, larguraBarra - 2, alturaBarra - 2, cor);
      }
      int larguraBarra = 160, alturaBarra = 14;
      int startX = (TELA_LARGURA - larguraBarra) / 2;
      int startY = TELA_ALTURA - 45;
      int preenchimento = ((larguraBarra - 6) * percentual) / 100;
      if (preenchimento > 0) {
        tft.fillRect(startX + 3, startY + 3, preenchimento, alturaBarra - 6, cor);
      }
    } else {
      int textoId = (estadoAtual == TELA_AVISO) ? (20 + indiceLembreteAtual) : 30;
      if (textoId != ultimoTextoId) {
        ultimoTextoId = textoId;
        tft.fillRect(0, FACE_Y + ms(LOG_H) + 4, TELA_LARGURA,
                     TELA_ALTURA - (FACE_Y + ms(LOG_H) + 4), COR_FUNDO);
        tft.setTextSize(2);
        tft.setTextColor(COR_TEXTO);
        const char* t = (estadoAtual == TELA_AVISO)
                          ? NOMES_LEMBRETE[indiceLembreteAtual]
                          : "Tudo tranquilo";
        int largura = (int)strlen(t) * 12;
        tft.setCursor((TELA_LARGURA - largura) / 2, TELA_ALTURA - 90);
        tft.print(t);
      }
    }
  }
}

void executarReset() {
  tone(PINO_BUZZER, 1800, 100);
  estadoAtual = TELA_LOGO;
  tempoInicioEstado = millis();
  ultimoTextoId = -1;
  alertaAtivo = false;
  estudando = false;
  tft.fillScreen(COR_FUNDO);
  bootEyes(millis());
  setMoodEyes(FELIZ);
  idleOn = false;
}

void alternarModoEstudo(unsigned long agora) {
  estudando = !estudando;
  ultimoTextoId = -1;
  if (estudando) {
    inicioEstudoMs = agora;
    ultimoSegundoMostrado = -1;
    idleOn = false;
    setMoodEyes(SONOLENTO);
    tone(PINO_BUZZER, 1200, 60);
  } else {
    unsigned long segundosTotais = (agora - inicioEstudoMs) / 1000;
    Serial.printf("Modo estudo: encerrado. Tempo total: %lum %lus\n",
                  segundosTotais / 60, segundosTotais % 60);
    tone(PINO_BUZZER, 800, 100);
    estadoAtual = TELA_TRANQUILO;
    tempoInicioEstado = agora;
    setMoodEyes(NEUTRO);
    idleOn = true;
  }
}

void tocarAlerta() {
  alertaAtivo = true;
  alertaInicioMs = millis();
  alertaFase = 0;
  tone(PINO_BUZZER, 1000, 150);
}
