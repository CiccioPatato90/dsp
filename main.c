#include "raylib.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>

#define HISTORY_LEN 800

typedef struct {
    float* items;
    size_t capacity;
    size_t count;
} Function;

#define da_append(list, value)                                                 \
  do {                                                                         \
    if ((list).count >= ((list)).capacity) {                                   \
      if ((list).capacity == 0) {                                              \
        (list).capacity = 256;                                                 \
      } else {                                                                 \
        (list).capacity *= 2;                                                  \
      }                                                                        \
      (list).items = realloc((list).items, (list).capacity * sizeof(*(list).items)); \
    }                                                                          \
    (list).items[(list).count++] = (value);                                      \
  } while (0);

int main(void) {
    const int screenWidth = 800;
    const int screenHeight = 800;
    InitWindow(screenWidth, screenHeight, "Function Drawing");


    float target_freq = 5.0f; // Start with 5Hz
    float sampling_rate = 320.0f; // This matches your SetTargetFPS

    float current_angle = 0.0f;
    float y_pos = (float) screenHeight/2;

    Function function = {0};
    da_append(function, y_pos);


    SetTargetFPS(sampling_rate);

    while (!WindowShouldClose()) {
        if (IsKeyDown(KEY_UP)) target_freq += 0.1f;
        if (IsKeyDown(KEY_DOWN)) target_freq -= 0.1f;
        if (target_freq < 0.1f) target_freq = 0.1f;

        // PRODUCER //
        // Formula: (2 * PI * freq) / sampling_rate
        float delta = (2.0f * PI * target_freq) / sampling_rate;
        current_angle += delta;
        if (current_angle > 2.0f * PI) current_angle -= 2.0f * PI;

        // Map sin value (-1 to 1) to screen coordinates
        // Center of screen is (400, 225). We multiply by 100 to make the movement visible.
        y_pos = 225 + (sin(current_angle) * 100);

        if (function.count >= screenWidth) {
            function.count = 0; // "Restart" by resetting the counter
        }

        da_append(function, y_pos);


        BeginDrawing();
            ClearBackground(RAYWHITE);
            DrawText(TextFormat("Frequency: %.1f Hz", target_freq), 20, 20, 20, DARKGRAY);
            DrawText("Use UP/DOWN arrows to change frequency", 20, 50, 15, GRAY);

            // CONSUMER //
            for (int i = 1; i < function.count; i++) {
                    // Define the start point (previous)
                    Vector2 start = { (float)(i - 1), function.items[i - 1] };
                    // Define the end point (current)
                    Vector2 end   = { (float)i, function.items[i] };
                    // Draw the segment
                    DrawLineV(start, end, SKYBLUE);
                }

                // Optional: Keep the "leading edge" dot so you can see where it's currently drawing
                if (function.count > 0) {
                    DrawCircle(function.count - 1, function.items[function.count - 1], 3, BLUE);
                }

        EndDrawing();
    }

    CloseWindow();
    return 0;
}
