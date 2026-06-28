#include <variant>
#include "FlexComponent.h"

class Text : public FlexComponent {

  struct NoWrap {};
  struct Truncate {
    int maxWidth;
  };
  struct Wrap {
    int maxWidth;
    int maxHeight;
  };

  using Mode = std::variant<NoWrap, Truncate, Wrap>;

  private:
    Mode mode;
    const char *text;

  public:
    Text(const char *text, Mode mode) : text(text), mode(mode) {};
};
