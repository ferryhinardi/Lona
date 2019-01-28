open Node;

[@bs.val] [@bs.module "fs-extra"] external ensureDirSync: string => unit = "";

[@bs.val] [@bs.module "fs-extra"]
external copySync: (string, string) => unit = "";

[@bs.module] external getStdin: unit => Js_promise.t(string) = "get-stdin";

let arguments = Array.to_list(Process.argv);

let positionalArguments =
  arguments |> List.filter(arg => !Js.String.startsWith("--", arg));

let getArgument = name => {
  let prefix = "--" ++ name ++ "=";
  switch (arguments |> List.find(Js.String.startsWith(prefix))) {
  | value =>
    Some(value |> Js.String.sliceToEnd(~from=Js.String.length(prefix)))
  | exception Not_found => None
  };
};

let swiftOptions: Swift.Options.options = {
  framework:
    switch (getArgument("framework")) {
    | Some("appkit") => Swift.Options.AppKit
    | _ => Swift.Options.UIKit
    },
  debugConstraints:
    switch (getArgument("debugConstraints")) {
    | Some(_) => true
    | _ => false
    },
  generateCollectionView:
    switch (getArgument("generateCollectionView")) {
    | Some(_) => true
    | _ => false
    },
  typePrefix:
    switch (getArgument("typePrefix")) {
    | Some(value) => value
    | _ => ""
    },
};

let javaScriptOptions: JavaScriptOptions.options = {
  framework:
    switch (getArgument("framework")) {
    | Some("reactsketchapp") => JavaScriptOptions.ReactSketchapp
    | Some("reactdom") => JavaScriptOptions.ReactDOM
    | _ => JavaScriptOptions.ReactNative
    },
  styleFramework:
    switch (getArgument("styleFramework")) {
    | Some("styledcomponents") => JavaScriptOptions.StyledComponents
    | _ => JavaScriptOptions.None
    },
};

let options: LonaCompilerCore.Options.options = {
  preset:
    switch (getArgument("preset")) {
    | Some("airbnb") => Airbnb
    | _ => Standard
    },
  filterComponents: getArgument("filterComponents"),
  swift: swiftOptions,
  javaScript: javaScriptOptions,
};

let exit = message => {
  Js.log(message);
  %bs.raw
  {|process.exit(1)|};
};

if (List.length(positionalArguments) < 3) {
  exit("No command given");
};

let command = List.nth(positionalArguments, 2);

if (command != "convertSvg" && List.length(positionalArguments) < 4) {
  exit("No target given");
};

let target =
  if (command != "convertSvg") {
    switch (List.nth(positionalArguments, 3)) {
    | "js" => Types.JavaScript
    | "swift" => Types.Swift
    | "xml" => Types.Xml
    | _ => exit("Unrecognized target")
    };
  } else {
    Types.JavaScript;
  };

let platformId =
  switch (target) {
  | Types.JavaScript =>
    switch (javaScriptOptions.framework) {
    | JavaScriptOptions.ReactDOM => Types.ReactDOM
    | JavaScriptOptions.ReactNative => Types.ReactNative
    | JavaScriptOptions.ReactSketchapp => Types.ReactSketchapp
    }
  | Types.Swift =>
    switch (swiftOptions.framework) {
    | SwiftOptions.UIKit => Types.IOS
    | SwiftOptions.AppKit => Types.MacOS
    }
  | Types.Xml =>
    /* TODO: Replace when we add android */
    Types.IOS
  };

let concat = (base, addition) => Path.join([|base, addition|]);

let getTargetExtension =
  fun
  | Types.JavaScript => ".js"
  | Swift => ".swift"
  | Xml => ".xml";

let formatFilename = (target, filename) =>
  switch (target) {
  | Types.Xml
  | Types.JavaScript => Format.camelCase(filename)
  | Types.Swift => Format.upperFirst(Format.camelCase(filename))
  };

let targetExtension = getTargetExtension(target);

let renderColors = (target, config: Config.t) =>
  switch (target) {
  | Types.Swift => Swift.Color.render(config)
  | JavaScript => JavaScript.Color.render(config.colorsFile.contents)
  | Xml => Xml.Color.render(config.colorsFile.contents)
  };

let renderTextStyles = (target, config: Config.t) =>
  switch (target) {
  | Types.Swift => Swift.TextStyle.render(config)
  | JavaScript =>
    JavaScriptTextStyle.render(
      config.options.javaScript,
      config.colorsFile.contents,
      config.textStylesFile.contents,
    )
  | _ => ""
  };

let renderShadows = (target, config: Config.t) =>
  switch (target) {
  | Types.Swift => SwiftShadow.render(config)
  | JavaScript =>
    JavaScriptShadow.render(
      config.options.javaScript,
      config.colorsFile.contents,
      config.shadowsFile.contents,
    )
  | _ => ""
  };

/* TODO: Update this. SwiftTypeSystem.render now returns a different format */
let convertTypes = (target, contents) => {
  let json = contents |> Js.Json.parseExn;
  switch (target) {
  | Types.Swift =>
    json
    |> TypeSystem.Decode.typesFile
    |> SwiftTypeSystem.render(swiftOptions)
  | _ => exit("Can't generate types for target")
  };
};

let convertColors = (target, config: Config.t) =>
  renderColors(target, config);

let convertTextStyles = (target, config: Config.t) =>
  renderTextStyles(target, config);

let convertShadows = (target, config: Config.t) =>
  renderShadows(target, config);

exception ComponentNotFound(string);

let findComponentFile = (fromDirectory, componentName) => {
  let searchPath = "**/" ++ componentName ++ ".component";
  let files = Glob.sync(concat(fromDirectory, searchPath)) |> Array.to_list;
  switch (List.length(files)) {
  | 0 => raise(ComponentNotFound(componentName))
  | _ => List.hd(files)
  };
};

let findComponent = (fromDirectory, componentName) => {
  let filename = findComponentFile(fromDirectory, componentName);
  let contents = Fs.readFileSync(filename, `utf8);
  contents |> Js.Json.parseExn;
};

let getComponentRelativePath =
    (fromDirectory, sourceComponent, importedComponent) => {
  let sourcePath =
    Node.Path.dirname(findComponentFile(fromDirectory, sourceComponent));
  let importedPath = findComponentFile(fromDirectory, importedComponent);
  let relativePath =
    Node.Path.relative(~from=sourcePath, ~to_=importedPath, ());
  Js.String.startsWith(".", relativePath) ?
    relativePath : "./" ++ relativePath;
};

let getAssetRelativePath = (fromDirectory, sourceComponent, importedPath) => {
  let sourcePath =
    Node.Path.dirname(findComponentFile(fromDirectory, sourceComponent));
  let importedPath = Node.Path.join([|fromDirectory, importedPath|]);
  let relativePath =
    Node.Path.relative(~from=sourcePath, ~to_=importedPath, ());
  Js.String.startsWith(".", relativePath) ?
    relativePath : "./" ++ relativePath;
};

let convertComponent =
    (config: Config.t, filename: string, outputFile: string) => {
  let contents = Fs.readFileSync(filename, `utf8);
  let parsed = contents |> Js.Json.parseExn;
  let name = Node.Path.basename_ext(filename, ".component");

  switch (target) {
  | Types.JavaScript =>
    JavaScriptComponent.generate(
      javaScriptOptions,
      name,
      Node.Path.relative(
        ~from=Node.Path.dirname(filename),
        ~to_=config.colorsFile.path,
        (),
      ),
      Node.Path.relative(
        ~from=Node.Path.dirname(filename),
        ~to_=config.shadowsFile.path,
        (),
      ),
      Node.Path.relative(
        ~from=Node.Path.dirname(filename),
        ~to_=config.textStylesFile.path,
        (),
      ),
      config,
      outputFile,
      getComponentRelativePath(config.workspacePath, name),
      getAssetRelativePath(config.workspacePath, name),
      parsed,
    )
    |> JavaScript.Render.toString
  | Swift =>
    let result =
      Swift.Component.generate(config, options, swiftOptions, name, parsed);
    result |> Swift.Render.toString;
  | _ => exit("Unrecognized target")
  };
};

let copyStaticFiles = outputDirectory =>
  switch (target) {
  | Types.Swift =>
    let staticFiles =
      ["TextStyle", "CGSize+Resizing", "LonaViewModel"]
      @ (
        switch (swiftOptions.framework) {
        | UIKit => ["LonaControlView", "Shadow"]
        | AppKit => ["LNATextField", "LNAImageView", "NSImage+Resizing"]
        }
      );

    let frameworkExtension =
      switch (swiftOptions.framework) {
      | AppKit => "appkit"
      | UIKit => "uikit"
      };

    staticFiles
    |> List.iter(file =>
         copySync(
           concat(
             [%bs.raw {| __dirname |}],
             "static/swift/" ++ file ++ "." ++ frameworkExtension ++ ".swift",
           ),
           concat(outputDirectory, file ++ ".swift"),
         )
       );
  | Types.JavaScript =>
    switch (javaScriptOptions.framework) {
    | ReactDOM =>
      let staticFiles = ["createActivatableComponent", "focusUtils"];
      staticFiles
      |> List.iter(file =>
           copySync(
             concat(
               [%bs.raw {| __dirname |}],
               "static/javaScript/" ++ file ++ ".js",
             ),
             concat(outputDirectory, "utils/" ++ file ++ ".js"),
           )
         );
    | _ => ()
    }
  | _ => ()
  };

let findContentsAbove = contents => {
  let lines = contents |> Js.String.split("\n");
  let index =
    lines
    |> Js.Array.findIndex(line =>
         line |> Js.String.includes("LONA: KEEP ABOVE")
       );
  switch (index) {
  | (-1) => None
  | _ =>
    Some(
      (
        lines
        |> Js.Array.slice(~start=0, ~end_=index + 1)
        |> Js.Array.joinWith("\n")
      )
      ++ "\n\n",
    )
  };
};

let findContentsBelow = contents => {
  let lines = contents |> Js.String.split("\n");
  let index =
    lines
    |> Js.Array.findIndex(line =>
         line |> Js.String.includes("LONA: KEEP BELOW")
       );
  switch (index) {
  | (-1) => None
  | _ =>
    Some(
      "\n" ++ (lines |> Js.Array.sliceFrom(index) |> Js.Array.joinWith("\n")),
    )
  };
};

let convertWorkspace = (workspace, output) => {
  let fromDirectory = Path.resolve(workspace, "");
  let toDirectory = Path.resolve(output, "");

  Config.load(platformId, options, workspace, toDirectory)
  |> Js.Promise.then_((config: Config.t) => {
       ensureDirSync(toDirectory);

       let userTypes = config.userTypesFile.contents;

       let colorsOutputPath =
         concat(
           toDirectory,
           formatFilename(target, "Colors") ++ targetExtension,
         );
       Fs.writeFileSync(
         colorsOutputPath,
         renderColors(target, config),
         `utf8,
       );

       let textStylesOutputPath =
         concat(
           toDirectory,
           formatFilename(target, "TextStyles") ++ targetExtension,
         );
       Fs.writeFileSync(
         textStylesOutputPath,
         renderTextStyles(target, config),
         `utf8,
       );

       if (target == Types.Swift || target == Types.JavaScript) {
         let shadowsOutputPath =
           concat(
             toDirectory,
             formatFilename(target, "Shadows") ++ targetExtension,
           );
         Fs.writeFileSync(
           shadowsOutputPath,
           renderShadows(target, config),
           `utf8,
         );
       };

       if (target == Types.Swift) {
         userTypes
         |> UserTypes.TypeSystem.toTypeSystemFile
         |> SwiftTypeSystem.render(swiftOptions)
         |> List.iter((convertedType: SwiftTypeSystem.convertedType) => {
              let importStatement =
                switch (swiftOptions.framework) {
                | AppKit => "import AppKit\n\n"
                | UIKit => "import UIKit\n\n"
                };
              let outputPath =
                concat(
                  toDirectory,
                  formatFilename(target, convertedType.name)
                  ++ targetExtension,
                );
              Fs.writeFileSync(
                outputPath,
                importStatement ++ convertedType.contents,
                `utf8,
              );
            });
       };

       copyStaticFiles(toDirectory);

       let successfulComponentNames =
         Glob.sync(concat(fromDirectory, "**/*.component"))
         |> Array.to_list
         |> List.filter(file =>
              switch (options.filterComponents) {
              | Some(value) => Js.Re.test(file, Js.Re.fromString(value))
              | None => true
              }
            )
         |> List.map(file => {
              let fromRelativePath =
                Path.relative(~from=fromDirectory, ~to_=file, ());
              let toRelativePath =
                concat(
                  Path.dirname(fromRelativePath),
                  Path.basename_ext(fromRelativePath, ".component"),
                )
                ++ targetExtension;
              let outputPath = Path.join([|toDirectory, toRelativePath|]);
              Js.log(
                Path.join([|workspace, fromRelativePath|])
                ++ "=>"
                ++ Path.join([|output, toRelativePath|]),
              );
              switch (convertComponent(config, file, outputPath)) {
              | exception (Json_decode.DecodeError(reason)) =>
                Js.log("Failed to decode " ++ file);
                Js.log(reason);
                None;
              | exception (Decode.UnknownParameter(name)) =>
                Js.log("Unknown parameter: " ++ name);
                None;
              | exception (Decode.UnknownExprType(name)) =>
                Js.log("Unknown expr name: " ++ name);
                None;
              | exception e =>
                Js.log("Unknown error");
                Js.log(e);
                None;
              | contents =>
                ensureDirSync(Path.dirname(outputPath));
                let (contentsAbove, contentsBelow) =
                  switch (Fs.readFileAsUtf8Sync(outputPath)) {
                  | existing => (
                      findContentsAbove(existing),
                      findContentsBelow(existing),
                    )
                  | exception _ => (None, None)
                  };
                let contents =
                  switch (contentsAbove) {
                  | Some(contentsAbove) => contentsAbove ++ contents
                  | None => contents
                  };
                let contents =
                  switch (contentsBelow) {
                  | Some(contentsBelow) => contents ++ contentsBelow
                  | None => contents
                  };
                Fs.writeFileSync(outputPath, contents, `utf8);

                Some(Path.basename_ext(fromRelativePath, ".component"));
              };
            })
         |> Sequence.compact;

       if (target == Types.Swift
           && swiftOptions.framework == UIKit
           && swiftOptions.generateCollectionView) {
         Fs.writeFileSync(
           concat(toDirectory, "LonaCollectionView.swift"),
           SwiftCollectionView.generate(
             config,
             options,
             swiftOptions,
             successfulComponentNames,
           ),
           `utf8,
         );
       };

       Glob.glob(
         concat(fromDirectory, "**/*.png"),
         (_, files) => {
           let files = Array.to_list(files);
           let processFile = file => {
             let fromRelativePath =
               Path.relative(~from=fromDirectory, ~to_=file, ());
             let outputPath = Path.join([|toDirectory, fromRelativePath|]);
             Js.log(
               Path.join([|workspace, fromRelativePath|])
               ++ "=>"
               ++ Path.join([|output, fromRelativePath|]),
             );
             copySync(file, outputPath);
           };
           files |> List.iter(processFile);
         },
       );
       Js.Promise.resolve();
     });
};
switch (command) {
| "workspace" =>
  if (List.length(positionalArguments) < 5) {
    exit("No workspace path given");
  };
  if (List.length(positionalArguments) < 6) {
    exit("No output path given");
  };
  convertWorkspace(
    List.nth(positionalArguments, 4),
    List.nth(positionalArguments, 5),
  )
  |> ignore;
| "component" =>
  if (List.length(positionalArguments) < 5) {
    exit("No filename given");
  };
  let filename = List.nth(positionalArguments, 4);
  Config.load(platformId, options, filename, "")
  |> Js.Promise.then_(config => {
       convertComponent(config, filename) |> Js.log;
       Js.Promise.resolve();
     })
  |> ignore;
| "colors" =>
  let initialWorkspaceSearchPath =
    List.length(positionalArguments) < 5 ?
      Process.cwd() : List.nth(positionalArguments, 4);

  Config.load(platformId, options, initialWorkspaceSearchPath, "")
  |> Js.Promise.then_((config: Config.t) =>
       if (List.length(positionalArguments) < 5) {
         getStdin()
         |> Js.Promise.then_(contents => {
              let config = {
                ...config,
                colorsFile: {
                  path: "__stdin__",
                  contents: Color.parseFile(contents),
                },
              };

              convertColors(target, config) |> Js.log;

              Js.Promise.resolve();
            });
       } else {
         convertColors(target, config) |> Js.log;

         Js.Promise.resolve();
       }
     )
  |> ignore;
| "shadows" =>
  let initialWorkspaceSearchPath =
    List.length(positionalArguments) < 5 ?
      Process.cwd() : List.nth(positionalArguments, 4);

  Config.load(platformId, options, initialWorkspaceSearchPath, "")
  |> Js.Promise.then_((config: Config.t) =>
       if (List.length(positionalArguments) < 5) {
         getStdin()
         |> Js.Promise.then_(contents => {
              let config = {
                ...config,
                shadowsFile: {
                  path: "__stdin__",
                  contents: Shadow.parseFile(contents),
                },
              };

              convertShadows(target, config) |> Js.log;

              Js.Promise.resolve();
            });
       } else {
         convertShadows(target, config) |> Js.log;

         Js.Promise.resolve();
       }
     )
  |> ignore;
| "types" =>
  if (List.length(positionalArguments) < 5) {
    exit("No filename given");
  } else {
    let contents =
      Node.Fs.readFileSync(List.nth(positionalArguments, 4), `utf8);
    convertTypes(target, contents) |> Js.log;
  }
| "textStyles" =>
  let initialWorkspaceSearchPath =
    List.length(positionalArguments) < 5 ?
      Process.cwd() : List.nth(positionalArguments, 4);

  Config.load(platformId, options, initialWorkspaceSearchPath, "")
  |> Js.Promise.then_((config: Config.t) =>
       if (List.length(positionalArguments) < 5) {
         getStdin()
         |> Js.Promise.then_(contents => {
              let config = {
                ...config,
                textStylesFile: {
                  path: "__stdin__",
                  contents: TextStyle.parseFile(contents),
                },
              };

              convertTextStyles(target, config) |> Js.log;

              Js.Promise.resolve();
            });
       } else {
         convertTextStyles(target, config) |> Js.log;

         Js.Promise.resolve();
       }
     )
  |> ignore;
| "convertSvg" =>
  let contents =
    if (List.length(positionalArguments) < 4) {
      getStdin();
    } else {
      Js.Promise.resolve(
        Node.Fs.readFileSync(List.nth(positionalArguments, 3), `utf8),
      );
    };
  Js.Promise.(
    contents
    |> then_(Svg.parse)
    |> then_(parsed => parsed |> Js.Json.stringify |> Js.log |> resolve)
    |> ignore
  );
| _ => Js.log2("Invalid command", command)
};