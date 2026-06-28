class FlexComponent {
  public:
    virtual ~FlexComponent() {};

    virtual void render() = 0;
    virtual int height() = 0;
    virtual int width() = 0;
};
