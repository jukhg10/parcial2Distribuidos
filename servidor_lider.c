#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdint.h>
#include <errno.h> 
#include "sha256.h"

// --- Configuración del Líder ---
#define PORT 65432
const char* DIFICULTAD = "000000"; //  ceros
#define NOMBRE_ARCHIVO "Archivo.txt" // Nombre del archivo a leer
#define MAX_BUFFER 1024
#define MAX_WORKERS 50 
// Tamaño del "carril" de trabajo para cada worker. 5 mil millones.
#define CHUNK_SIZE 5000000000LL 
// ---------------------------------

// --- Variables Globales ---
// Búfer para almacenar los datos del archivo
char g_datos_del_bloque[MAX_BUFFER];

// Sincronización de la solución
int g_solucion_encontrada = 0;
long long g_nonce_ganador;
pthread_mutex_t g_mutex_solucion = PTHREAD_MUTEX_INITIALIZER;

// Lista de clientes
int g_client_sockets[MAX_WORKERS];
int g_client_count = 0;
pthread_mutex_t g_mutex_clientes = PTHREAD_MUTEX_INITIALIZER;

// Contador global para asignar rangos de trabajo
long long g_rango_actual = 0;
pthread_mutex_t g_mutex_rango = PTHREAD_MUTEX_INITIALIZER;
// ---------------------------------

/**
 * @brief Lee el contenido de un archivo de texto en un búfer.
 */
int leer_archivo_txt(const char* nombre_archivo, char* buffer, size_t buffer_size) {
    FILE* archivo = fopen(nombre_archivo, "r");
    if (archivo == NULL) {
        fprintf(stderr, "ERROR: No se pudo abrir el archivo '%s'. Error: %s\n", nombre_archivo, strerror(errno));
        return -1;
    }

    size_t bytes_leidos = fread(buffer, 1, buffer_size - 1, archivo);
    if (bytes_leidos == 0 && !feof(archivo)) {
        fprintf(stderr, "ERROR: No se pudo leer el archivo '%s'.\n", nombre_archivo);
        fclose(archivo);
        return -1;
    }

    buffer[bytes_leidos] = '\0';
    
    // Quitar saltos de línea al final
    if (bytes_leidos > 0 && buffer[bytes_leidos - 1] == '\n') {
        buffer[bytes_leidos - 1] = '\0';
    }

    fclose(archivo);
    printf("Datos cargados desde '%s': \"%s\"\n", nombre_archivo, buffer);
    return 0;
}

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
 * @brief Verifica si un nonce dado produce un hash válido.
 */
int verificar_pow(const char* datos, const char* dificultad, long long nonce) {
    // Aumentar tamaño para seguridad: datos + nonce string
    char texto_a_probar[MAX_BUFFER + 128]; 
    snprintf(texto_a_probar, sizeof(texto_a_probar), "%s%lld", datos, nonce);
    
    unsigned char hash_bytes[32];
    char hash_hex[65];
    
    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (unsigned char*)texto_a_probar, strlen(texto_a_probar));
    sha256_final(&ctx, hash_bytes);
    
    hash_a_string(hash_bytes, hash_hex);
    
    return strncmp(hash_hex, dificultad, strlen(dificultad)) == 0;
}

/**
 * @brief Remueve un cliente de la lista global g_client_sockets.
 */
void remover_cliente(int client_socket) {
    pthread_mutex_lock(&g_mutex_clientes);
    for (int i = 0; i < g_client_count; i++) {
        if (g_client_sockets[i] == client_socket) {
            // Mover el último elemento a esta posición para llenar el hueco
            g_client_sockets[i] = g_client_sockets[g_client_count - 1];
            g_client_count--;
            break;
        }
    }
    pthread_mutex_unlock(&g_mutex_clientes);
}

/**
 * @brief Notifica "PERDISTE" a todos los workers excepto al ganador.
 */
void notificar_perdedores(int socket_ganador) {
    pthread_mutex_lock(&g_mutex_clientes);
    for (int i = 0; i < g_client_count; i++) {
        if (g_client_sockets[i] != socket_ganador) {
            send(g_client_sockets[i], "PERDISTE", 8, 0);
        }
    }
    // Limpiar la lista para la próxima ronda
    g_client_count = 0;
    pthread_mutex_unlock(&g_mutex_clientes);
}

/**
 * @brief Función ejecutada por un hilo para manejar a un solo worker.
 */
void* manejar_worker(void* arg) {
    int client_socket = (intptr_t)arg;
    char buffer[MAX_BUFFER];
    
    printf("Worker conectado. Socket: %d\n", client_socket);

    // --- Asignar un rango de trabajo único ---
    long long rango_inicio;
    
    pthread_mutex_lock(&g_mutex_rango);
    rango_inicio = g_rango_actual;
    g_rango_actual += CHUNK_SIZE;
    pthread_mutex_unlock(&g_mutex_rango);
    
    printf("Asignando rango a worker %d: Iniciar en %lld\n", client_socket, rango_inicio);
    // ------------------------------------------------

    // 1. Enviar trabajo (Formato: "Datos|Dificultad|RangoInicio")
    char trabajo[MAX_BUFFER + 128]; // Búfer seguro
    snprintf(trabajo, sizeof(trabajo), "%s|%s|%lld", g_datos_del_bloque, DIFICULTAD, rango_inicio);
    send(client_socket, trabajo, strlen(trabajo), 0);

    // 3. Esperar solución (nonce)
    memset(buffer, 0, MAX_BUFFER);
    int bytes_leidos = read(client_socket, buffer, MAX_BUFFER - 1);

    if (bytes_leidos <= 0) {
        printf("Worker %d se desconectó.\n", client_socket);
        remover_cliente(client_socket);
        close(client_socket);
        return NULL;
    }
    
    // Revisar si alguien ganó mientras este worker enviaba su respuesta
    pthread_mutex_lock(&g_mutex_solucion);
    if (g_solucion_encontrada) {
        pthread_mutex_unlock(&g_mutex_solucion);
        send(client_socket, "TARDE", 5, 0);
        remover_cliente(client_socket);
        close(client_socket);
        return NULL;
    }
    pthread_mutex_unlock(&g_mutex_solucion);

    long long nonce = atoll(buffer);
    printf("Worker %d propone nonce: %lld\n", client_socket, nonce);

    // 4. Verificar la solución
    if (verificar_pow(g_datos_del_bloque, DIFICULTAD, nonce)) {
        
        // --- Sección Crítica: Reclamar Victoria ---
        pthread_mutex_lock(&g_mutex_solucion);
        if (!g_solucion_encontrada) {
            // ¡GANADOR!
            g_solucion_encontrada = 1;
            g_nonce_ganador = nonce;
            
            send(client_socket, "GANASTE", 7, 0);
            printf("-----------------------------------\n");
            printf("¡GANADOR ENCONTRADO! Worker: %d\n", client_socket);
            printf("Nonce: %lld\n", g_nonce_ganador);
            printf("-----------------------------------\n");
            
            // Notificar a todos los demás
            notificar_perdedores(client_socket);

        } else {
            // Alguien ganó mientras verificábamos
            send(client_socket, "TARDE", 5, 0);
        }
        pthread_mutex_unlock(&g_mutex_solucion);
        // --- Fin Sección Crítica ---

    } else {
        // El worker envió un nonce inválido
        send(client_socket, "FALLO", 5, 0);
    }

    remover_cliente(client_socket);
    close(client_socket);
    return NULL;
}

/**
 * @brief Función principal del servidor.
 */
int main() {
    int server_fd, client_socket;
    struct sockaddr_in address;
    int opt = 1;
    socklen_t addrlen = sizeof(address);

    // --- Cargar datos del archivo antes de empezar ---
    if (leer_archivo_txt(NOMBRE_ARCHIVO, g_datos_del_bloque, MAX_BUFFER) != 0) {
        fprintf(stderr, "Error al cargar los datos del bloque. Abortando.\n");
        exit(EXIT_FAILURE);
    }
    // ---------------------------------------------------------

    // --- Configuración del Socket del Servidor ---
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Fallo al crear socket");
        exit(EXIT_FAILURE);
    }
    
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Fallo en bind");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 10) < 0) {
        perror("Fallo en listen");
        exit(EXIT_FAILURE);
    }

    printf("Líder escuchando en el puerto %d con dificultad '%s'\n", PORT, DIFICULTAD);

    // --- Bucle principal para aceptar workers ---
    while (1) {
        if ((client_socket = accept(server_fd, (struct sockaddr *)&address, &addrlen)) < 0) {
            perror("Fallo en accept");
            continue;
        }
        
        // Revisar si la ronda ya terminó
        pthread_mutex_lock(&g_mutex_solucion);
        if (g_solucion_encontrada) {
            pthread_mutex_unlock(&g_mutex_solucion);
            send(client_socket, "RONDA_TERMINADA", 15, 0);
            close(client_socket);
        } else {
            pthread_mutex_unlock(&g_mutex_solucion);
            
            // Revisar si hay espacio para más workers
            pthread_mutex_lock(&g_mutex_clientes);
            if (g_client_count < MAX_WORKERS) {
                g_client_sockets[g_client_count++] = client_socket;
                pthread_mutex_unlock(&g_mutex_clientes);

                // Crear hilo para manejar al worker
                pthread_t tid;
                if (pthread_create(&tid, NULL, manejar_worker, (void*)(intptr_t)client_socket) != 0) {
                    perror("Fallo al crear pthread");
                    remover_cliente(client_socket); // Limpiar si el hilo no se crea
                    close(client_socket);
                }
                pthread_detach(tid); // El hilo correrá independientemente
            } else {
                // Servidor lleno
                pthread_mutex_unlock(&g_mutex_clientes);
                send(client_socket, "SERVIDOR_LLENO", 14, 0);
                close(client_socket);
            }
        }
    }

    // Limpieza final (aunque el bucle es infinito)
    close(server_fd);
    pthread_mutex_destroy(&g_mutex_solucion);
    pthread_mutex_destroy(&g_mutex_clientes);
    pthread_mutex_destroy(&g_mutex_rango);
    return 0;
}