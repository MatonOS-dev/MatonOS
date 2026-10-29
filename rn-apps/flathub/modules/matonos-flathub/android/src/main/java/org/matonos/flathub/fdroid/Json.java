package org.matonos.flathub.fdroid;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Small dependency-free JSON reader for the signed repository catalogue. */
final class Json {
  private final String input;
  private int offset;

  private Json(String input) { this.input = input; }

  static Object parse(String input) {
    Json parser = new Json(input);
    Object value = parser.value();
    parser.space();
    if (parser.offset != input.length()) throw new IllegalArgumentException("Unexpected JSON data");
    return value;
  }

  private Object value() {
    space();
    if (offset >= input.length()) throw new IllegalArgumentException("Unexpected end of JSON");
    char next = input.charAt(offset);
    if (next == '{') return object();
    if (next == '[') return array();
    if (next == '"') return string();
    if (next == 't') return literal("true", Boolean.TRUE);
    if (next == 'f') return literal("false", Boolean.FALSE);
    if (next == 'n') return literal("null", null);
    return number();
  }

  private Map<String, Object> object() {
    Map<String, Object> result = new LinkedHashMap<>(); offset++;
    space(); if (take('}')) return result;
    do {
      space(); if (offset >= input.length() || input.charAt(offset) != '"') throw new IllegalArgumentException("Expected object key");
      String key = string(); space(); require(':'); result.put(key, value()); space();
      if (take('}')) return result;
      require(',');
    } while (true);
  }

  private List<Object> array() {
    List<Object> result = new ArrayList<>(); offset++; space(); if (take(']')) return result;
    do { result.add(value()); space(); if (take(']')) return result; require(','); } while (true);
  }

  private String string() {
    require('"'); StringBuilder result = new StringBuilder();
    while (offset < input.length()) {
      char c = input.charAt(offset++);
      if (c == '"') return result.toString();
      if (c != '\\') { result.append(c); continue; }
      if (offset >= input.length()) throw new IllegalArgumentException("Bad JSON escape");
      char escaped = input.charAt(offset++);
      switch (escaped) {
        case '"': case '\\': case '/': result.append(escaped); break;
        case 'b': result.append('\b'); break; case 'f': result.append('\f'); break;
        case 'n': result.append('\n'); break; case 'r': result.append('\r'); break; case 't': result.append('\t'); break;
        case 'u':
          if (offset + 4 > input.length()) throw new IllegalArgumentException("Bad unicode escape");
          result.append((char) Integer.parseInt(input.substring(offset, offset + 4), 16)); offset += 4; break;
        default: throw new IllegalArgumentException("Bad JSON escape");
      }
    }
    throw new IllegalArgumentException("Unterminated JSON string");
  }

  private Object number() {
    int start = offset;
    while (offset < input.length() && "-+0123456789.eE".indexOf(input.charAt(offset)) >= 0) offset++;
    if (start == offset) throw new IllegalArgumentException("Expected JSON value");
    String value = input.substring(start, offset);
    try { return value.contains(".") || value.contains("e") || value.contains("E") ? Double.valueOf(value) : Long.valueOf(value); }
    catch (NumberFormatException error) { throw new IllegalArgumentException("Bad JSON number", error); }
  }

  private Object literal(String token, Object value) { if (!input.startsWith(token, offset)) throw new IllegalArgumentException("Bad JSON literal"); offset += token.length(); return value; }
  private void space() { while (offset < input.length() && Character.isWhitespace(input.charAt(offset))) offset++; }
  private boolean take(char c) { if (offset < input.length() && input.charAt(offset) == c) { offset++; return true; } return false; }
  private void require(char c) { if (!take(c)) throw new IllegalArgumentException("Expected '" + c + "'"); }
}
