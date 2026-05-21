#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "dynbuf.h"

// Инициализация буфера: устанавливаем нулевые значения.
// Если buf == NULL, просто возвращаем.
void dynbuf_init(dynbuf_t *buf) {
	if (!buf) return;
	buf->data = NULL;
	buf->len = 0;
	buf->cap = 0;
}

// Освобождение памяти буфера.
void dynbuf_free(dynbuf_t *buf) {
	if (!buf) return;
	free(buf->data);
	buf->data = NULL;
	buf->len = 0;
	buf->cap = 0;
}

// Резервирование дополнительного пространства: гарантируем, что после buf->len + additional <= buf->cap.
// Если буфер не инициализирован (buf == NULL), возвращаем ошибку.
// Защита от переполнения: проверяем, чтобы buf->len + additional не выходило за SIZE_MAX.
// Используем стратегию роста: cap *= 2 или начинаем с 1024 байт до требуемого.
int dynbuf_reserve(dynbuf_t *buf, size_t additional) {
	if (!buf) return -1;
	// Проверяем переполнение
	if (additional > SIZE_MAX - buf->len) {
		return -1;
	}
	size_t need = buf->len + additional;
	if (need <= buf->cap) {
		return 0;
	}
	// Выбираем новую ёмкость
	size_t newcap = buf->cap ? buf->cap * 2 : 1024;
	// Если newcap == 0 (buf->cap == 0), newcap = 1024. Далее удваиваем, пока newcap < need.
	while (newcap < need) {
		// Проверяем переполнение при удвоении
		if (newcap > SIZE_MAX / 2) {
			newcap = need;  // прямо ставим нужный размер
			break;
		}
		newcap *= 2;
	}
	// Выполняем realloc. Если buf->data == NULL, эквивалентно malloc.
	void *p = realloc(buf->data, newcap);
	if (!p) {
		return -1;
	}
	buf->data = (char *)p;
	buf->cap = newcap;
	return 0;
}

// Добавление данных в конец буфера. Если buf == NULL или reserve неудачно — возвращаем -1.
// Если len == 0, ничего не делаем и возвращаем 0.
int dynbuf_append(dynbuf_t *buf, const void *data, size_t len) {
	if (!buf) return -1;
	if (len == 0) return 0;
	if (!data) return -1;
	if (dynbuf_reserve(buf, len) != 0) {
		return -1;
	}
	// Теперь buf->data указывает на выделенный участок длины cap >= buf->len + len
	memcpy(buf->data + buf->len, data, len);
	buf->len += len;
	return 0;
}

