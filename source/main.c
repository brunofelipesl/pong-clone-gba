
#include <gba_console.h>
#include <gba_video.h>
#include <gba_interrupt.h>
#include <gba_systemcalls.h>
#include <gba_input.h>
#include <stdio.h>
#include <stdlib.h>

#define MEM_VRAM 0x06000000
#define MEM_PALETTE 0x05000000
#define REG_DISPCNT_ADDR 0x04000000
#define MODE4_BACKBUFFER 0x0010
#define MODE4_PAGE_SIZE 0xA000
#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 160

typedef volatile u16 M4LINE[SCREEN_WIDTH / 2];
#define palette_mem ((volatile u16*)MEM_PALETTE)
#define reg_dispcnt (*(volatile u16*)REG_DISPCNT_ADDR)

volatile u16* backBuffer = (volatile u16*)(MEM_VRAM + MODE4_PAGE_SIZE);

#define GAME_STATE_PLAYING 0
#define GAME_STATE_ENDED 1
#define RESULT_DURATION_FRAMES 300

#define COLOR_BLACK 0
#define COLOR_RED 1
#define COLOR_BLUE 2
#define COLOR_WHITE 3

struct rect{
	int x;
	int y;
	int width;
	int height;
	int velocityX;
	int velocityY;
	int prevX;
	int prevY;
};

struct gameState {
	int playerScore;
	int aiScore;
	int timerFrames;  // 180 frames = 3 segundos (60 FPS)
	int totalSeconds; // 180 segundos = 3 minutos
	int state;        // GAME_STATE_PLAYING ou GAME_STATE_ENDED
	int winner;       // 0 = player, 1 = ai, 2 = tie
	int resultFrames; // Contador para mostrar resultado por 5 segundos
};

void drawPixel(int x, int y, u16 color) {
	if (x >= 0 && x < SCREEN_WIDTH && y >= 0 && y < SCREEN_HEIGHT) {
		int offset = y * (SCREEN_WIDTH / 2) + (x / 2);
		u16 pixelPair = backBuffer[offset];
		if (x & 1) {
			backBuffer[offset] = (pixelPair & 0x00FF) | (color << 8);
		} else {
			backBuffer[offset] = (pixelPair & 0xFF00) | color;
		}
	}
}

void clearScreen(u16 color) {
	u16 pixelPair = color | (color << 8);
	for (int y = 0; y < SCREEN_HEIGHT; y++) {
		for (int x = 0; x < SCREEN_WIDTH / 2; x++) {
			backBuffer[y * (SCREEN_WIDTH / 2) + x] = pixelPair;
		}
	}
}

void setupPalette() {
	palette_mem[COLOR_BLACK] = RGB5(0, 0, 0);
	palette_mem[COLOR_RED] = RGB5(31, 0, 0);
	palette_mem[COLOR_BLUE] = RGB5(0, 0, 31);
	palette_mem[COLOR_WHITE] = RGB5(31, 31, 31);
}

void flipBuffers() {
	VBlankIntrWait();
	reg_dispcnt ^= MODE4_BACKBUFFER;
	if (reg_dispcnt & MODE4_BACKBUFFER) {
		backBuffer = (volatile u16*)MEM_VRAM;
	} else {
		backBuffer = (volatile u16*)(MEM_VRAM + MODE4_PAGE_SIZE);
	}
}

void drawRect(struct rect* cRect, u16 color) {
	for (int i= cRect-> x; i < cRect->x + cRect -> width; i++) {
		for (int j = cRect->y; j < cRect->y + cRect->height; j++) {
			drawPixel(i, j, color);
		}
	}
}

// Detecção de colisão AABB (Axis-Aligned Bounding Box)
int checkCollision(struct rect* a, struct rect* b) {
	return (a->x < b->x + b->width &&
	        a->x + a->width > b->x &&
	        a->y < b->y + b->height &&
	        a->y + a->height > b->y);
}

void clampRectToScreen(struct rect* cRect) {
	if (cRect->x < 0) {
		cRect->x = 0;
	} else if (cRect->x > SCREEN_WIDTH - cRect->width) {
		cRect->x = SCREEN_WIDTH - cRect->width;
	}

	if (cRect->y < 0) {
		cRect->y = 0;
	} else if (cRect->y > SCREEN_HEIGHT - cRect->height) {
		cRect->y = SCREEN_HEIGHT - cRect->height;
	}
}

// Desenha a linha do meio
void drawMiddleLine() {
	for (int y = 0; y < SCREEN_HEIGHT; y++) {
		if (y % 5 < 3) {  // Cria um padrão tracejado
			drawPixel(SCREEN_WIDTH / 2, y, COLOR_WHITE); // White
		}
	}
}

// Desenha um dígito (0-9) em uma posição específica - versão simples 3x5
void drawDigit(int digit, int x, int y, u16 color) {
	// Padrões de dígitos 3x5 de forma visual clara
	uint8_t patterns[10][5] = {
		{0x7, 0x5, 0x5, 0x5, 0x7},  // 0
		{0x2, 0x2, 0x2, 0x2, 0x2},  // 1
		{0x7, 0x1, 0x7, 0x4, 0x7},  // 2
		{0x7, 0x1, 0x7, 0x1, 0x7},  // 3
		{0x5, 0x5, 0x7, 0x1, 0x1},  // 4
		{0x7, 0x4, 0x7, 0x1, 0x7},  // 5
		{0x7, 0x4, 0x7, 0x5, 0x7},  // 6
		{0x7, 0x1, 0x2, 0x4, 0x4},  // 7
		{0x7, 0x5, 0x7, 0x5, 0x7},  // 8
		{0x7, 0x5, 0x7, 0x1, 0x7},  // 9
	};
	
	for (int j = 0; j < 5; j++) {
		for (int i = 0; i < 3; i++) {
			if (patterns[digit][j] & (1 << (2 - i))) {
				drawPixel(x + i, y + j, color);
			}
		}
	}
}

// Desenha um número com múltiplos dígitos
void drawNumber(int number, int x, int y, u16 color) {
	if (number < 10) {
		drawDigit(number, x, y, color);
	} else {
		drawDigit(number / 10, x, y, color);
		drawDigit(number % 10, x + 4, y, color);
	}
}

// Desenha o tempo em formato MM:SS com separador
void drawTimer(int totalSeconds, int x, int y, u16 color) {
	int minutes = totalSeconds / 60;
	int seconds = totalSeconds % 60;
	
	// Desenhar minutos
	drawDigit(minutes, x, y, color);
	
	// Desenhar separador ":"
	drawPixel(x + 4, y + 1, color);
	drawPixel(x + 4, y + 3, color);
	
	// Desenhar segundos (com zero à esquerda se necessário)
	if (seconds < 10) {
		drawDigit(0, x + 6, y, color);
		drawDigit(seconds, x + 10, y, color);
	} else {
		drawDigit(seconds / 10, x + 6, y, color);
		drawDigit(seconds % 10, x + 10, y, color);
	}
}

// Desenha texto simples (strings pré-definidas)
void drawSimpleText(const char* text, int x, int y, u16 color) {
	int pos = 0;
	while (text[pos] != '\0' && pos < 50) {
		char c = text[pos];
		// Desenha caracteres simples como blocos de pixel
		if (c == ' ') {
			// Espaço em branco
		} else if (c >= '0' && c <= '9') {
			drawDigit(c - '0', x + pos * 6, y, color);
		} else if (c == 'V' || c == 'v') {
			// V
			drawPixel(x + pos * 6, y, color);
			drawPixel(x + pos * 6 + 4, y, color);
			drawPixel(x + pos * 6 + 1, y + 1, color);
			drawPixel(x + pos * 6 + 3, y + 1, color);
			drawPixel(x + pos * 6 + 2, y + 2, color);
		} else if (c == 'E' || c == 'e') {
			// E
			drawPixel(x + pos * 6, y, color);
			drawPixel(x + pos * 6 + 1, y, color);
			drawPixel(x + pos * 6 + 2, y, color);
			drawPixel(x + pos * 6, y + 1, color);
			drawPixel(x + pos * 6 + 1, y + 1, color);
			drawPixel(x + pos * 6, y + 2, color);
			drawPixel(x + pos * 6 + 1, y + 2, color);
			drawPixel(x + pos * 6 + 2, y + 2, color);
		} else if (c == 'M' || c == 'm') {
			// M
			for (int i = 0; i < 3; i++) {
				drawPixel(x + pos * 6, y + i, color);
				drawPixel(x + pos * 6 + 4, y + i, color);
			}
			drawPixel(x + pos * 6 + 1, y, color);
			drawPixel(x + pos * 6 + 2, y + 1, color);
			drawPixel(x + pos * 6 + 3, y, color);
		} else if (c == 'P' || c == 'p') {
			// P
			for (int i = 0; i < 3; i++) {
				drawPixel(x + pos * 6, y + i, color);
			}
			drawPixel(x + pos * 6 + 1, y, color);
			drawPixel(x + pos * 6 + 2, y, color);
			drawPixel(x + pos * 6 + 2, y + 1, color);
			drawPixel(x + pos * 6 + 1, y + 1, color);
		} else if (c == 'A' || c == 'a') {
			// A
			drawPixel(x + pos * 6 + 2, y, color);
			drawPixel(x + pos * 6 + 1, y + 1, color);
			drawPixel(x + pos * 6 + 2, y + 1, color);
			drawPixel(x + pos * 6 + 3, y + 1, color);
			drawPixel(x + pos * 6, y + 2, color);
			drawPixel(x + pos * 6 + 4, y + 2, color);
		} else if (c == 'I' || c == 'i') {
			// I
			for (int i = 0; i < 3; i++) {
				drawPixel(x + pos * 6 + 1, y + i, color);
			}
		} else if (c == 'U' || c == 'u') {
			// U
			for (int i = 0; i < 2; i++) {
				drawPixel(x + pos * 6, y + i, color);
				drawPixel(x + pos * 6 + 4, y + i, color);
			}
			drawPixel(x + pos * 6 + 1, y + 2, color);
			drawPixel(x + pos * 6 + 2, y + 2, color);
			drawPixel(x + pos * 6 + 3, y + 2, color);
		} else if (c == 'T' || c == 't') {
			// T
			for (int i = 0; i < 3; i++) {
				drawPixel(x + pos * 6 + 1, y + i, color);
			}
			drawPixel(x + pos * 6, y, color);
			drawPixel(x + pos * 6 + 2, y, color);
		} else if (c == 'R' || c == 'r') {
			// R
			for (int i = 0; i < 3; i++) {
				drawPixel(x + pos * 6, y + i, color);
			}
			drawPixel(x + pos * 6 + 1, y, color);
			drawPixel(x + pos * 6 + 2, y, color);
			drawPixel(x + pos * 6 + 2, y + 1, color);
			drawPixel(x + pos * 6 + 1, y + 1, color);
			drawPixel(x + pos * 6 + 2, y + 2, color);
		} else if (c == 'W' || c == 'w') {
			// W
			for (int i = 0; i < 3; i++) {
				drawPixel(x + pos * 6, y + i, color);
				drawPixel(x + pos * 6 + 4, y + i, color);
			}
			drawPixel(x + pos * 6 + 1, y + 2, color);
			drawPixel(x + pos * 6 + 2, y + 1, color);
			drawPixel(x + pos * 6 + 3, y + 2, color);
		} else if (c == 'I' || c == 'i') {
			// I
			for (int i = 0; i < 3; i++) {
				drawPixel(x + pos * 6 + 1, y + i, color);
			}
		} else if (c == 'N' || c == 'n') {
			// N
			for (int i = 0; i < 3; i++) {
				drawPixel(x + pos * 6, y + i, color);
				drawPixel(x + pos * 6 + 4, y + i, color);
			}
			drawPixel(x + pos * 6 + 1, y, color);
			drawPixel(x + pos * 6 + 2, y + 1, color);
			drawPixel(x + pos * 6 + 3, y + 2, color);
		} else if (c == 'S' || c == 's') {
			// S
			drawPixel(x + pos * 6 + 1, y, color);
			drawPixel(x + pos * 6 + 2, y, color);
			drawPixel(x + pos * 6, y + 1, color);
			drawPixel(x + pos * 6 + 1, y + 1, color);
			drawPixel(x + pos * 6 + 3, y + 1, color);
			drawPixel(x + pos * 6 + 4, y + 2, color);
		}
		pos++;
	}
}

int main(void) {

	irqInit();
	irqEnable(IRQ_VBLANK);

	SetMode(MODE_4 | BG2_ENABLE);
	setupPalette();

	struct gameState game;
	game.playerScore = 0;
	game.aiScore = 0;
	game.timerFrames = 60 * 180;  // 3 minutos a 60 FPS = 10800 frames
	game.totalSeconds = 180;      // 3 minutos em segundos
	game.state = GAME_STATE_PLAYING;
	game.winner = 0;
	game.resultFrames = 0;

	struct rect humanPlayer;
	humanPlayer.x = 1;
	humanPlayer.y = SCREEN_HEIGHT/2 - 12;
	humanPlayer.prevX = humanPlayer.x;
	humanPlayer.prevY = humanPlayer.y;
	humanPlayer.width = 8;
	humanPlayer.height = 24;
	humanPlayer.velocityX = 0;
	humanPlayer.velocityY = 0;


	struct rect aiPlayer;
	aiPlayer.x = SCREEN_WIDTH - 9;
	aiPlayer.y = SCREEN_HEIGHT/2 - 12;
	aiPlayer.prevX = aiPlayer.x;
	aiPlayer.prevY = aiPlayer.y;
	aiPlayer.width = 8;
	aiPlayer.height = 24;
	aiPlayer.velocityX = 0;
	aiPlayer.velocityY = 0;


    struct rect ball;
	ball.x = SCREEN_WIDTH/2 - 4;	
	ball.y = SCREEN_HEIGHT/2 - 4;
	ball.prevX = ball.x;
	ball.prevY = ball.y;
	ball.width = 8;
	ball.height = 8;
	ball.velocityX = -4;  // Bola começa se movendo para a esquerda
	ball.velocityY = 2;


	clearScreen(COLOR_BLACK);
	flipBuffers();
	clearScreen(COLOR_BLACK);

	while (1) {
		if (game.state == GAME_STATE_ENDED) {
			// Mostrar resultado por 5 segundos
			game.resultFrames++;
			if (game.resultFrames < RESULT_DURATION_FRAMES) {
				// Continue mostrando o resultado
			} else {
				// Reiniciar o jogo
				game.playerScore = 0;
				game.aiScore = 0;
				game.timerFrames = 60 * 180;
				game.totalSeconds = 180;
				game.state = GAME_STATE_PLAYING;
				game.resultFrames = 0;
				ball.x = SCREEN_WIDTH / 2 - 4;
				ball.y = SCREEN_HEIGHT / 2 - 4;
				ball.velocityX = -4;
				ball.velocityY = 2;
				humanPlayer.y = SCREEN_HEIGHT / 2 - 12;
				aiPlayer.y = SCREEN_HEIGHT / 2 - 12;
				humanPlayer.velocityY = 0;
				aiPlayer.velocityY = 0;
				humanPlayer.prevX = humanPlayer.x;
				humanPlayer.prevY = humanPlayer.y;
				aiPlayer.prevX = aiPlayer.x;
				aiPlayer.prevY = aiPlayer.y;
				ball.prevX = ball.x;
				ball.prevY = ball.y;
			// Limpar texto de resultado quando reinicia
			for (int i = 30; i < 210; i++) {
				for (int j = 70; j < 85; j++) {
					drawPixel(i, j, COLOR_BLACK);
				}
			}
		}
	}

	if (game.state == GAME_STATE_PLAYING) {
			scanKeys();
			int key_pressed = keysDown();
			int key_released = keysUp();

			if((key_released & KEY_UP) || (key_released & KEY_DOWN)) {
				humanPlayer.velocityY = 0;
			}

			if((key_pressed & KEY_UP) && humanPlayer.y > 0) {
				humanPlayer.velocityY = -4;
			}

			if((key_pressed & KEY_DOWN) && humanPlayer.y < SCREEN_HEIGHT - humanPlayer.height) {
				humanPlayer.velocityY = 4;
			}

			if((humanPlayer.y <= 0 && humanPlayer.velocityY < 0) || (humanPlayer.y >= SCREEN_HEIGHT - humanPlayer.height && humanPlayer.velocityY > 0)) {
				humanPlayer.velocityY = 0;
			}

			humanPlayer.y += humanPlayer.velocityY;
			clampRectToScreen(&humanPlayer);

			// Lógica do oponente (IA) - segue a bola
			int aiSpeed = 4;
			int aiCenter = aiPlayer.y + aiPlayer.height / 2;
			
			if (aiCenter < ball.y && aiPlayer.y < SCREEN_HEIGHT - aiPlayer.height) {
				aiPlayer.y += aiSpeed;
			} else if (aiCenter > ball.y && aiPlayer.y > 0) {
				aiPlayer.y -= aiSpeed;
			}
			clampRectToScreen(&aiPlayer);

			// Movimento da bola
			ball.x += ball.velocityX;
			ball.y += ball.velocityY;

			// Colisão da bola com as bordas superior e inferior
			if (ball.y <= 0 || ball.y >= SCREEN_HEIGHT - ball.height) {
				ball.velocityY = -ball.velocityY;
				clampRectToScreen(&ball);
			}

			// Colisão da bola com o jogador humano
			if (checkCollision(&ball, &humanPlayer)) {
				ball.velocityX = -ball.velocityX;
				ball.x = humanPlayer.x + humanPlayer.width;
			}

			// Colisão da bola com o oponente (IA)
			if (checkCollision(&ball, &aiPlayer)) {
				ball.velocityX = -ball.velocityX;
				ball.x = aiPlayer.x - ball.width;
			}

			// Detecção de gol e pontuação
			if (ball.x < 0) {
				game.aiScore++;
				ball.x = SCREEN_WIDTH / 2 - 4;
				ball.y = SCREEN_HEIGHT / 2 - 4;
				ball.velocityX = -4;
				ball.velocityY = 2;
				if (game.aiScore >= 10) {
					game.state = GAME_STATE_ENDED;
					game.winner = 1;  // AI venceu
				}
			} else if (ball.x > SCREEN_WIDTH) {
				game.playerScore++;
				ball.x = SCREEN_WIDTH / 2 - 4;
				ball.y = SCREEN_HEIGHT / 2 - 4;
				ball.velocityX = 4;
				ball.velocityY = 2;
				if (game.playerScore >= 10) {
					game.state = GAME_STATE_ENDED;
					game.winner = 0;  // Player venceu
				}
			}

			// Atualizar timer
			game.timerFrames--;
			if (game.timerFrames % 60 == 0) {
				game.totalSeconds--;
			}

			// Verificar fim de tempo (3 minutos)
			if (game.totalSeconds <= 0) {
				game.state = GAME_STATE_ENDED;
				if (game.playerScore > game.aiScore) {
					game.winner = 0;  // Player venceu
				} else if (game.aiScore > game.playerScore) {
					game.winner = 1;  // AI venceu
				} else {
					game.winner = 2;  // Empate
				}
			}
		}

		clearScreen(COLOR_BLACK);

		// Limpar área superior para placar e timer
		// Redesenhar linha do meio (sobre a área anterior)
		drawMiddleLine();

		// Desenhar objetos
		drawRect(&humanPlayer, COLOR_RED); // Red color
		drawRect(&aiPlayer, COLOR_BLUE); // Blue color
		drawRect(&ball, COLOR_WHITE); // White color

		// Desenhar placar
		drawNumber(game.playerScore, 10, 2, COLOR_RED);  // Placar do jogador (vermelho)
		drawNumber(game.aiScore, 210, 2, COLOR_BLUE);     // Placar da IA (azul)

		// Desenhar timer no meio (centralizado)
		drawTimer(game.totalSeconds, 105, 2, COLOR_WHITE);

		// Se o jogo acabou, mostrar resultado
		if (game.state == GAME_STATE_ENDED && game.resultFrames < RESULT_DURATION_FRAMES) {
			// Limpar área de resultado
			for (int i = 30; i < 210; i++) {
				for (int j = 70; j < 85; j++) {
					drawPixel(i, j, COLOR_BLACK);
				}
			}
			// Desenhar texto de resultado
			if (game.winner == 0) {
				drawSimpleText("PLAYER RED WINS", 50, 75, COLOR_RED);
			} else if (game.winner == 1) {
				drawSimpleText("AI BLUE WINS", 70, 75, COLOR_BLUE);
			} else {
				drawSimpleText("TIE", 105, 75, COLOR_WHITE);
			}
		}
	
		humanPlayer.prevY = humanPlayer.y;
		humanPlayer.prevX = humanPlayer.x;
		aiPlayer.prevX = aiPlayer.x;
		aiPlayer.prevY = aiPlayer.y;
		ball.prevX = ball.x;
		ball.prevY = ball.y;

		flipBuffers();
	}
}
