extern double ordinary_external(double);

/* Input without assembly labels must remain identical with the plugin loaded. */
double ordinary_caller(double x) { return ordinary_external(x) + 1.0; }
