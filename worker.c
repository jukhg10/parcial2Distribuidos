#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h> // Necesario para hilos
#include <time.h>    // Aunque no usamos srand, lo dejamos por si acaso
#include "sha256.h"

// --- Configuración del Worker ---
const char* HOST_LIDER = "127.0.0.1";
#define PORT_LIDER 65432
#define MAX_BUFFER 1024
// ---------------------------------

// Variable global para detener el minado
volatile int g_stop_mining = 0;

// Estructura para pasar datos al hilo minero
typedef struct {
    int socket;
    char datos[512];
    char dificultad[128];
    long long rango_inicio; // Rango de inicio asignado por el líder
} MinarDatos;


/**
 * @brief Convierte 32 bytes de hash en 65 bytes de string hexadecimal.
 */
void hash_a_string(unsigned char hash_bytes[32], char hash_hex[65]) {
    for (int i = 0; i < 32; i++) {
        sprintf(&hash_hex[i * 2], "%02x", hash_bytes[i]);
    }
    hash_hex[64] = '\0';
}

/**
 * @brief Función de minado (fuerza bruta) que corre en un hilo.
 */
void* minar_en_hilo(void* arg) {
    MinarDatos* config = (MinarDatos*)arg;
    
    // MODIFICADO: ¡Ya no es aleatorio!
    long long nonce = config->rango_inicio;
    printf("¡Empezando a minar en el rango asignado: %lld!\n", nonce);

    char texto_a_probar[MAX_BUFFER]; // Búfer local para minar
    unsigned char hash_bytes[32];
    char hash_hex[65];
    size_t dificultad_len = strlen(config->dificultad);

    // Bucle principal de minado
    while (!g_stop_mining) { // Comprueba la bandera global
        snprintf(texto_a_probar, MAX_BUFFER, "%s%lld", config->datos, nonce);

        SHA256_CTX ctx;
        sha256_init(&ctx);
        sha256_update(&ctx, (unsigned char*)texto_a_probar, strlen(texto_a_probar));
        sha256_final(&ctx, hash_bytes);
        hash_a_string(hash_bytes, hash_hex);

        // Compara si el hash cumple la dificultad
        if (strncmp(hash_hex, config->dificultad, dificultad_len) == 0) {
            printf("\n¡Solución encontrada! Nonce: %lld\n", nonce);
            printf("Hash: %s\n", hash_hex);
            
            // Enviar solución
            char solucion_str[32];
            snprintf(solucion_str, 32, "%lld", nonce);
            send(config->socket, solucion_str, strlen(solucion_str), 0);
            
            break; // Salir del bucle y terminar el hilo
        }

        // Imprimir de vez en cuando (y chequear bandera)
        if (nonce % 100000 == 0) {
            if (g_stop_mining) break; // Chequeo extra
            printf("\rProbando nonce: %lld...", nonce);
            fflush(stdout);
        }

        nonce++; // Avanza al siguiente nonce en su "carril"
    }
    
    printf("\nHilo minero detenido.\n");
    return NULL;
}

/**
 * @brief Parsea el paquete de trabajo (3 partes) enviado por el líder.
 */
void parsear_trabajo(char* trabajo, char* datos_out, char* dificultad_out, long long* inicio_out) {
    char* token = strtok(trabajo, "|");
    if (token != NULL) {
        strcpy(datos_out, token);
    }
    
    token = strtok(NULL, "|");
    if (token != NULL) {
        strcpy(dificultad_out, token);
    }

    // Parsear la tercera parte: rango de inicio
    token = strtok(NULL, "|");
    if (token != NULL) {
        *inicio_out = atoll(token); // Convertir texto a long long
    }
}

/**
 * @brief Función principal del worker.
 */
int main() {
    // Ya no necesitamos inicializar la semilla aleatoria
    // srand(time(NULL) ^ getpid()); 

    int sock = 0;
    struct sockaddr_in serv_addr;
    char buffer[MAX_BUFFER] = {0};

    // --- 1. Conexión al Servidor ---
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("\nError al crear socket");
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT_LIDER);

    if (inet_pton(AF_INET, HOST_LIDER, &serv_addr.sin_addr) <= 0) {
        perror("Dirección inválida");
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Conexión fallida. ¿El Líder está encendido?");
        return -1;
    }
    printf("Conectado al Líder en %s:%d\n", HOST_LIDER, PORT_LIDER);

    // --- 2. Recibir Trabajo ---
    int bytes_leidos = read(sock, buffer, MAX_BUFFER - 1);
    if (bytes_leidos <= 0) {
        perror("El Líder cerró la conexión");
        close(sock);
        return -1;
    }
    buffer[bytes_leidos] = '\0'; // Asegurar fin de string

    // Manejar rechazo del servidor
    if (strcmp(buffer, "RONDA_TERMINADA") == 0 || strcmp(buffer, "SERVIDOR_LLENO") == 0) {
        printf("Respuesta del Líder: %s\n", buffer);
        close(sock);
        return 0;
    }

    // Preparar datos para el hilo minero
    MinarDatos datos_minado;
    datos_minado.socket = sock;
    parsear_trabajo(buffer, datos_minado.datos, datos_minado.dificultad, &datos_minado.rango_inicio);
    
    printf("Recibido: Datos='%s', Dificultad='%s', Rango de inicio: %lld\n", 
           datos_minado.datos, datos_minado.dificultad, datos_minado.rango_inicio);
    
    // --- 3. Crear Hilo Minero ---
    pthread_t tid_minero;
    if (pthread_create(&tid_minero, NULL, minar_en_hilo, &datos_minado) != 0) {
        perror("Fallo al crear hilo minero");
        close(sock);
        return -1;
    }

    // --- 4. Hilo Principal Escucha por Notificaciones ---
    printf("Hilo principal escuchando por notificaciones...\n");
    memset(buffer, 0, MAX_BUFFER);
    
    // read() es una llamada BLOQUEANTE. Se detendrá aquí hasta que reciba datos.
    bytes_leidos = read(sock, buffer, MAX_BUFFER - 1);
    
    // En cuanto reciba ALGO (un mensaje o una desconexión), detenemos el minado
    g_stop_mining = 1;

    if (bytes_leidos > 0) {
        buffer[bytes_leidos] = '\0';
        if (strcmp(buffer, "PERDISTE") == 0) {
            printf("\n\n------------------------\n");
            printf("¡Otro worker ganó! Deteniendo.\n");
            printf("------------------------\n\n");
        } else {
            // "GANASTE", "TARDE", "FALLO"
            printf("\n\nRespuesta del Líder (recibida después de enviar): %s\n", buffer);
        }
    } else {
        // bytes_leidos <= 0
        printf("\n\nSe perdió la conexión con el líder.\n");
    }

    // --- 5. Limpieza ---
    // Esperar a que el hilo minero termine
    pthread_join(tid_minero, NULL);
    printf("Programa worker terminado.\n");

    close(sock);
    return 0;
}