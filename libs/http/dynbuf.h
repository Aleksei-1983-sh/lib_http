
#ifndef HTTP_DYNBUF_H
#define HTTP_DYNBUF_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/// Opaque динамический буфер
typedef struct dynbuf {
	char *data;    ///< указатель на данные
	size_t   len;     ///< текущая длина (занято)
	size_t   cap;     ///< общая ёмкость буфера
} dynbuf_t;



// Инициализация буфера: устанавливаем нулевые значения.
// Если buf == NULL, просто возвращаем.
void dynbuf_init(dynbuf_t *buf);



// Освобождение памяти буфера.
void dynbuf_free(dynbuf_t *buf);


// Резервирование дополнительного пространства: гарантируем, что после buf->len + additional <= buf->cap.
// Если буфер не инициализирован (buf == NULL), возвращаем ошибку.
// Защита от переполнения: проверяем, чтобы buf->len + additional не выходило за SIZE_MAX.
// Используем стратегию роста: cap *= 2 или начинаем с 1024 байт до требуемого.
int dynbuf_reserve(dynbuf_t *buf, size_t additional);


// Добавление данных в конец буфера. Если buf == NULL или reserve неудачно — возвращаем -1.
// Если len == 0, ничего не делаем и возвращаем 0.
int dynbuf_append(dynbuf_t *buf, const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif // HTTP_DYNBUF_H
