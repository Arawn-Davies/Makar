# types - Common type aliases and geometric structs

**Header:** `kernel/include/kernel/types.h`

Provides a small set of type aliases and simple structs used throughout the
kernel.

---

## Type aliases

### `string`

```c
typedef char *string;
```

Alias for `char *`.  Used in older parts of the codebase (e.g. the serial
driver) as a semantic hint that the pointer points to a null-terminated
C string.

---

## Structs

### `Point`

```c
typedef struct {
    int32_t X, Y;
} Point;
```

A signed 2-D integer coordinate.  Suitable for screen positions where
negative values are meaningful (e.g. partially off-screen).

### `UPoint`

```c
typedef struct {
    uint32_t X, Y;
} UPoint;
```

An unsigned 2-D integer coordinate.  Suitable for pixel or character-cell
positions where coordinates are always non-negative.
