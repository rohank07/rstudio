/*
 * Copyright 2008 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy of
 * the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations under
 * the License.
 */
package com.google.gwt.uibinder.elementparsers;

import com.google.gwt.core.ext.UnableToCompleteException;
import com.google.gwt.uibinder.rebind.UiBinderWriter;
import com.google.gwt.uibinder.rebind.XMLElement;

import java.util.Collections;
import java.util.HashMap;
import java.util.Map;

/**
 * Used by {@link HTMLPanelParser} to interpret elements that call for widget
 * instances. Declares the appropriate widget, and replaces them in the markup
 * with a &lt;span&gt;.
 */
class WidgetInterpreter implements XMLElement.Interpreter<String> {

  private static final Map<String, String> LEGAL_CHILD_ELEMENTS;
  private static final String DEFAULT_CHILD_ELEMENT = "span";
  static {
    // Other cases?
    HashMap<String, String> map = new HashMap<String, String>();
    map.put("table", "tbody");
    map.put("thead", "tr");
    map.put("tbody", "tr");
    map.put("tfoot", "tr");
    map.put("ul", "li");
    map.put("ol", "li");
    map.put("dl", "dt");
    LEGAL_CHILD_ELEMENTS = Collections.unmodifiableMap(map);
  }

  private static String getLegalPlaceholderTag(XMLElement elem) {
    XMLElement parent = elem.getParent();
    String tag = null;
    if (parent != null) {
      tag = LEGAL_CHILD_ELEMENTS.get(parent.getLocalName());
    }
    if (tag == null) {
      tag = DEFAULT_CHILD_ELEMENT;
    }
    return tag;
  }

  private final String fieldName;

  private final UiBinderWriter uiWriter;

  public WidgetInterpreter(String fieldName, UiBinderWriter writer) {
    this.fieldName = fieldName;
    this.uiWriter = writer;
  }

  public String interpretElement(XMLElement elem) 
      throws UnableToCompleteException {
    if (uiWriter.isWidgetElement(elem)) { 
      
      String tag = getLegalPlaceholderTag(elem);
      
      String childField = uiWriter.parseElementToField(elem);
      
      String elementPointer = "element" + uiWriter.getUniqueId();
      uiWriter.addInitStatement(
          "com.google.gwt.user.client.Element %s = %s;",
          elementPointer, uiWriter.getDomAccessExpression(elementPointer, tag));

      uiWriter.addInitStatement(
          "%1$s.addAndReplaceElement(%2$s, %3$s);", 
          fieldName, childField, elementPointer);      

      
      // Increment DOM cursor based on the tag we are adding.
      uiWriter.getCurrentDomCursor().advanceChild();
      // Create an element to hold the widget.
      return "<" + tag + "></" + tag + ">";
    }
    return null;
  }
}
