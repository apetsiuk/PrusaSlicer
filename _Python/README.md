# Multi-Material Layer Batching Algorithm



| Source Model  | Printing Map | Printing Graph |
| ------------- | ------------- | ------------- |
| <img width="300" src="images/test_tsp_model.png"/>  | <img width="300" src="images/printing_map_csv.png"/>  | <img width="300" src="images/fabrication_graph.png"/>  |
| Source model represent a number of layers divided into colored `printing regions`  | Printing map CSV file is generated during slicing process, where `*R` column represent region overlay between neighboring regions  | This CSV file can be represented as a graph for further printing optimization. Each edge rpresents a single tool change.  |