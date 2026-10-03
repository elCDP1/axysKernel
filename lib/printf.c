#include "axys/printf.h"
#include "axys/console.h"
#include "axys/string.h"

struct output_buffer {
    char *buffer;
    axys_size_t size;
    axys_size_t length;
};

static void output_character(struct output_buffer *output, char character)
{
    /* The length counter saturates rather than wrapping: if it could reach
     * SIZE_MAX then `length + 1` would wrap to 0 and defeat the bound check,
     * turning an over-long format string into an out-of-bounds write. */
    if (output->buffer != AXYS_NULL && output->length < output->size &&
        output->length + 1 < output->size) {
        output->buffer[output->length] = character;
    }
    if (output->length != (axys_size_t)-1) {
        ++output->length;
    }
}

static void output_string(struct output_buffer *output, const char *string)
{
    while (*string != '\0') {
        output_character(output, *string);
        ++string;
    }
}

static void output_repeated(struct output_buffer *output, char character, axys_size_t count)
{
    for (axys_size_t index = 0; index < count; ++index) {
        output_character(output, character);
    }
}

static axys_size_t number_digits(axys_uint64_t value, axys_uint32_t base)
{
    axys_size_t digits = 1;

    while (value >= base) {
        value /= base;
        ++digits;
    }
    return digits;
}

static char digit_character(axys_uint64_t value, axys_uint32_t base, int uppercase)
{
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    return digits[value % base];
}

static void output_number(struct output_buffer *output, axys_uint64_t value, axys_uint32_t base,
                           int uppercase, char sign, const char *prefix, axys_size_t prefix_length,
                           int width, int zero_pad, int left_align)
{
    axys_size_t sign_length = sign == '\0' ? 0 : 1;
    axys_size_t digit_count = number_digits(value, base);
    axys_size_t number_length = sign_length + prefix_length + digit_count;
    axys_size_t padding = width > (int)number_length ? (axys_size_t)width - number_length : 0;

    if (!left_align && !zero_pad) {
        output_repeated(output, ' ', padding);
    }
    if (sign != '\0') {
        output_character(output, sign);
    }
    if (prefix != AXYS_NULL) {
        output_string(output, prefix);
    }
    if (!left_align && zero_pad) {
        output_repeated(output, '0', padding);
    }

    axys_uint64_t divisor = 1;
    for (axys_size_t index = 1; index < digit_count; ++index) {
        divisor *= base;
    }
    for (axys_size_t index = 0; index < digit_count; ++index) {
        output_character(output, digit_character((value / divisor) % base, base, uppercase));
        divisor /= base;
    }
    if (left_align) {
        output_repeated(output, ' ', padding);
    }
}

static axys_size_t format_output(axys_va_list arguments, struct output_buffer *output, const char *format)
{
    while (*format != '\0') {
        int left_align = 0;
        int zero_pad = 0;
        int plus_sign = 0;
        int space_sign = 0;
        int alternate = 0;
        int width = 0;
        int length_modifier = 0;

        if (*format != '%') {
            output_character(output, *format);
            ++format;
            continue;
        }
        ++format;
        if (*format == '%') {
            output_character(output, '%');
            ++format;
            continue;
        }

        for (;;) {
            if (*format == '-') {
                left_align = 1;
            } else if (*format == '0') {
                zero_pad = 1;
            } else if (*format == '+') {
                plus_sign = 1;
            } else if (*format == ' ') {
                space_sign = 1;
            } else if (*format == '#') {
                alternate = 1;
            } else {
                break;
            }
            ++format;
        }
        while (*format >= '0' && *format <= '9') {
            int digit = *format - '0';

            /* Clamp before multiplying, so even a hostile number with many
             * digits never creates an oversized intermediate width. */
            if (width >= 4096 || width > (4096 - digit) / 10) {
                width = 4096;
            } else {
                width = width * 10 + digit;
            }
            ++format;
        }
        while (*format == 'l') {
            ++length_modifier;
            ++format;
        }
        if (*format == 'z') {
            length_modifier = 3;
            ++format;
        }

        switch (*format) {
        case 'c': {
            char character = (char)__builtin_va_arg(arguments, int);
            if (!left_align) {
                output_repeated(output, ' ', width > 1 ? (axys_size_t)(width - 1) : 0);
            }
            output_character(output, character);
            if (left_align) {
                output_repeated(output, ' ', width > 1 ? (axys_size_t)(width - 1) : 0);
            }
            break;
        }
        case 'd':
        case 'i': {
            axys_int64_t value;
            if (length_modifier == 0) {
                value = __builtin_va_arg(arguments, int);
            } else if (length_modifier == 1) {
                value = __builtin_va_arg(arguments, long);
            } else if (length_modifier == 2) {
                value = __builtin_va_arg(arguments, long long);
            } else {
                value = __builtin_va_arg(arguments, axys_intptr_t);
            }
            if (value < 0) {
                output_number(output, 0U - (axys_uint64_t)value, 10, 0, '-', AXYS_NULL, 0,
                              width, zero_pad, left_align);
            } else {
                output_number(output, (axys_uint64_t)value, 10, 0,
                              plus_sign ? '+' : (space_sign ? ' ' : '\0'), AXYS_NULL, 0,
                              width, zero_pad, left_align);
            }
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            axys_uint64_t value;
            if (length_modifier == 0) {
                value = __builtin_va_arg(arguments, unsigned int);
            } else if (length_modifier == 1) {
                value = __builtin_va_arg(arguments, unsigned long);
            } else if (length_modifier == 2) {
                value = __builtin_va_arg(arguments, unsigned long long);
            } else {
                value = __builtin_va_arg(arguments, axys_size_t);
            }
            if (alternate && (*format == 'x' || *format == 'X') && value != 0) {
                output_number(output, value, 16, *format == 'X', '\0', *format == 'X' ? "0X" : "0x", 2,
                              width, zero_pad, left_align);
            } else {
                output_number(output, value, (*format == 'u') ? 10 : 16, *format == 'X', '\0',
                              AXYS_NULL, 0, width, zero_pad, left_align);
            }
            break;
        }
        case 'p': {
            void *pointer = __builtin_va_arg(arguments, void *);
            output_number(output, (axys_uint64_t)(axys_uintptr_t)pointer,
                          16, 0, '\0', "0x", 2, 18, 1, 0);
            break;
        }
        case 's': {
            const char *string = __builtin_va_arg(arguments, const char *);
            axys_size_t string_length;
            if (string == AXYS_NULL) {
                string = "(null)";
            }
            string_length = axys_strlen(string);
            if (!left_align) {
                output_repeated(output, ' ', width > (int)string_length ? (axys_size_t)width - string_length : 0);
            }
            output_string(output, string);
            if (left_align) {
                output_repeated(output, ' ', width > (int)string_length ? (axys_size_t)width - string_length : 0);
            }
            break;
        }
        case '%':
            output_character(output, '%');
            break;
        default:
            /*
             * Either a real unknown conversion, or the format string ended
             * immediately after '%'. A trailing bare '%' used to fall into this
             * branch, emit a '%' plus the NUL terminator itself, and then be
             * followed by the unconditional `++format` below -- which advanced
             * one byte past the terminator and kept reading whatever followed
             * the string in memory. Always keep the literal '%', but only
             * echo the offending character when there actually is one.
             */
            output_character(output, '%');
            if (*format != '\0') {
                output_character(output, *format);
            }
            break;
        }
        if (*format == '\0') {
            break;
        }
        ++format;
    }
    return output->length;
}

int axys_vsnprintf(char *buffer, axys_size_t size, const char *format, axys_va_list arguments)
{
    struct output_buffer output = {
        .buffer = buffer,
        .size = size,
        .length = 0,
    };

    if (buffer != AXYS_NULL && size != 0) {
        buffer[0] = '\0';
    }
    if (format == AXYS_NULL) {
        format = "(null)";
    }
    axys_size_t length = format_output(arguments, &output, format);
    if (buffer != AXYS_NULL && size != 0) {
        axys_size_t terminator = output.length < size - 1 ? output.length : size - 1;
        buffer[terminator] = '\0';
    }
    return length > (axys_size_t)0x7fffffff ? 0x7fffffff : (int)length;
}

int axys_snprintf(char *buffer, axys_size_t size, const char *format, ...)
{
    axys_va_list arguments;
    int result;

    __builtin_va_start(arguments, format);
    result = axys_vsnprintf(buffer, size, format, arguments);
    __builtin_va_end(arguments);
    return result;
}

int axys_vprintf(const char *format, axys_va_list arguments)
{
    char buffer[512];
    int result = axys_vsnprintf(buffer, sizeof(buffer), format, arguments);

    axys_console_write_len(buffer, (axys_size_t)(result < (int)sizeof(buffer) ? result : (int)sizeof(buffer) - 1));
    return result;
}

int axys_printf(const char *format, ...)
{
    axys_va_list arguments;
    int result;

    __builtin_va_start(arguments, format);
    result = axys_vprintf(format, arguments);
    __builtin_va_end(arguments);
    return result;
}
