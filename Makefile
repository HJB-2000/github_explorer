# --- Compiler Configuration ---
CXX      := g++
CXXFLAGS := -std=c++11 -Wall -Wextra -O2 -MMD -MP

# --- Project Directories and Target ---
TARGET   := visualizer
SRC_DIR  := .
BUILD_DIR:= build
IMGUI_DIR:= ./imgui

# --- Include Paths ---
INC_FLAGS:= -I$(SRC_DIR) -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends

# --- Linker Flags & Libraries (Linux Setup) ---
PKG_CONFIG := $(shell command -v pkg-config 2>/dev/null)
ifeq ($(PKG_CONFIG),)
LIBS     := -lglfw -lGL -ldl -lpthread -lcurl
else
LIBS     := $(shell pkg-config --libs glfw3 libcurl) -lGL -ldl -lpthread
INC_FLAGS += $(shell pkg-config --cflags glfw3 libcurl)
endif

# --- Source Files ---
# Project sources
SRCS     := $(SRC_DIR)/main.cpp \
			$(SRC_DIR)/githubAPI.cpp

# ImGui Core
SRCS     += $(IMGUI_DIR)/imgui.cpp \
            $(IMGUI_DIR)/imgui_draw.cpp \
            $(IMGUI_DIR)/imgui_widgets.cpp \
            $(IMGUI_DIR)/imgui_tables.cpp \
            $(IMGUI_DIR)/imgui_demo.cpp

# ImGui Backends (GLFW + OpenGL3)
SRCS     += $(IMGUI_DIR)/backends/imgui_impl_glfw.cpp \
            $(IMGUI_DIR)/backends/imgui_impl_opengl3.cpp

# --- Object Files Mapping ---
# This maps source files to equivalent .o files inside the build directory
OBJS     := $(patsubst $(SRC_DIR)/%.cpp, $(BUILD_DIR)/%.o, $(SRCS))

# Generated dependency files
DEPS     := $(OBJS:.o=.d)

# --- VPATH Configuration ---
# Allows make to look for source dependencies in these directories automatically
vpath %.cpp $(SRC_DIR) $(IMGUI_DIR) $(IMGUI_DIR)/backends

# --- Build Rules ---

.PHONY: all clean fclean re run

all: $(TARGET)

# Link final executable
$(TARGET): $(OBJS)
	@echo "Linking final binary: $(TARGET)..."
	@$(CXX) $(CXXFLAGS) $(OBJS) -o $(TARGET) $(LIBS)
	@echo "Build successful! Run './$(TARGET)' to launch."

# Compile source files to object files
$(BUILD_DIR)/%.o: %.cpp | $(BUILD_DIR)
	@echo "Compiling: $<"
	@mkdir -p $(dir $@)
	@$(CXX) $(CXXFLAGS) $(INC_FLAGS) -c $< -o $@

# Create build directory if it does not exist
$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# Clean object files
clean:
	@echo "Removing object files..."
	@rm -rf $(BUILD_DIR)

# Full cleanup (objects + binary)
fclean: clean
	@echo "Removing $(TARGET)..."
	@rm -f $(TARGET)

# Rebuild project
re: fclean all

# Rebuild and launch a fresh instance of the app
run: re
	@echo "Launching fresh instance: ./$(TARGET)..."
	@./$(TARGET)

-include $(DEPS)