int isalpha(int c) { return (c>='a'&&c<='z')||(c>='A'&&c<='Z'); }
int isdigit(int c) { return c>='0'&&c<='9'; }
int isalnum(int c) { return isalpha(c)||isdigit(c); }
int isspace(int c) { return c==' '||c=='\t'||c=='\n'||c=='\v'||c=='\f'||c=='\r'; }
int iscntrl(int c) { return (unsigned)c < 32 || c == 127; }
int isxdigit(int c){ return isdigit(c)||(c>='a'&&c<='f')||(c>='A'&&c<='F'); }
int ispunct(int c) { return c>32 && c<127 && !isalnum(c); }
int isupper(int c) { return c>='A'&&c<='Z'; }
int islower(int c) { return c>='a'&&c<='z'; }
int isprint(int c) { return c>=32 && c<127; }
int isgraph(int c) { return c>32 && c<127; }
int tolower(int c) { return isupper(c) ? c+32 : c; }
int toupper(int c) { return islower(c) ? c-32 : c; }
