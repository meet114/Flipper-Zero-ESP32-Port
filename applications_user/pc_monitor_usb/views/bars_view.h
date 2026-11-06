#pragma once

#include <gui/view.h>

typedef struct BarsView BarsView;

BarsView* bars_view_alloc();
void bars_view_free(BarsView* bars_view);
View* bars_view_get_view(BarsView* bars_view);
void bars_view_set_data(BarsView* bars_view, float* data, size_t count);
