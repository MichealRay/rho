expected <- eval(parse(text="structure(\"integer(0)\", .Names = \"c0\", row.names = character(0))"));        
test(id=0, code={        
argv <- eval(parse(text="list(structure(\"integer(0)\", .Names = \"c0\", row.names = character(0)), 0L, \"\", 0L, TRUE)"));        
.Internal(encodeString(argv[[1]], argv[[2]], argv[[3]], argv[[4]], argv[[5]]));        
}, o=expected);        

