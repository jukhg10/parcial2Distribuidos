# parcial2Distribuidos

# Proyecto de Minería Proof-of-Work en C

Esta es una simulación de un sistema Cliente-Servidor de minería (Proof-of-Work) centralizada, escrito en C puro. El proyecto demuestra conceptos de sistemas distribuidos, multithreading (pthreads), sockets de red y criptografía básica.

## Características

* **Líder (Servidor):**
    * Multihilo (maneja múltiples workers a la vez).
    * Lee un `Archivo.txt` para definir el "bloque" a minar.
    * Define la dificultad (ej. "000000").
    * Asigna rangos de trabajo ("carriles") únicos a cada worker para evitar solapamiento.
    * Valida la solución y notifica al ganador y a los perdedores.
* **Worker (Cliente):**
    * Multihilo: un hilo mina (fuerza bruta) y el hilo principal escucha.
    * Recibe su "carril" de trabajo del líder (ej. "empezar en 5,000,000,000").
    * Busca el `nonce` que resuelva el desafío SHA-256.
    * Se detiene automáticamente si otro worker gana.
* **Algoritmo:** SHA-256 (implementado desde cero en `sha256.c`).

## Cómo Compilar

El proyecto usa `gcc` y la biblioteca `pthreads` (POSIX threads). Asegúrate de estar en un entorno Linux o WSL (Subsistema de Windows para Linux).

```bash
# Compilar el Servidor (Líder)
gcc -o servidor servidor_lider.c sha256.c -lpthread

# Compilar el Cliente (Worker)
gcc -o worker worker.c sha256.c -lpthread

## Cómo Ejecutar
Asegúrate de tener un archivo llamado Archivo.txt en la misma carpeta.

Inicia el servidor en una terminal:

Bash

./servidor
Inicia uno o más workers en terminales separadas:

Bash

./worker
